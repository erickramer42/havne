// File/layer: tests/test_j2534_surrogate.cpp | GTest, HARNESS tier
// Testing: spawns the REAL surrogate process hosting the fake vendor DLL,
//          and asserts frames cross the pipe intact: exact counts,
//          payload fidelity, RxStatus->flags translation, vendor
//          timestamp passthrough, FIFO order, no reads before CONNECT,
//          clean job-object teardown, and fast failure on a dead DLL.
//          Also: write->echo round trip, ioctl SET->GET + VBATT,
//          child-side ioctl whitelist, filter start/stop lifecycle,
//          recovery after a timed-out request, a blocked vendor open,
//          an ERR_TIMEOUT partial batch (spec-mode fake), and no
//          phantom frames from an untouched count under a hard error.
//
// ARCH note: in the x64 configure this is all-x64 (CI-valid IPC test);
// the true cross-arch case (x64 test process -> x86 surrogate -> x86
// fake) runs when QTTUNE_SURROGATE_EXE points at Win32-tree artifacts.
// The write/ioctl/filter assertions are deliberately observable
// through RETURN VALUES and frames read back over the pipe alone (no
// fake-instance hooks) so they hold in BOTH architectures unchanged —
// an x64 test cannot reach into the x86 child's fake instance.

#include <gtest/gtest.h>

#include "qttune/j2534_surrogate.h"
#include "qttune/core.h"
#include "qttune/j2534_ptypes.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <windows.h>

/* Env override for the cross-arch case: an x86 surrogate can't load
 * the x64 fake baked at generate time, so the Win32 tree's fake DLL
 * takes precedence when QTTUNE_FAKE_DLL is set. Mirrors the client's
 * QTTUNE_SURROGATE_EXE override. */
static const char* fake_dll_path()
{
    const char* env = std::getenv("QTTUNE_FAKE_DLL");
    return (env != nullptr && env[0] != '\0') ? env : FAKE_J2534_DLL_PATH;
}

namespace {

struct SurCapture {
    std::atomic<int> count{0};
    unsigned char payloads[8][64]{};
    unsigned int frame_flags[8]{};
    uint64_t timestamps[8]{};
};

void sur_capture_cb(const qttune_frame_t* frame, void* user_data)
{
    SurCapture* cap = static_cast<SurCapture*>(user_data);
    const int i = cap->count.fetch_add(1);
    if (i < 8) {
        memcpy(cap->payloads[i], frame->data, 64);
        cap->frame_flags[i] = static_cast<unsigned int>(frame->flags);
        cap->timestamps[i] = frame->timestamp_us;
    }
}

/* Poll-until-condition with deadline — no wall-clock bets: a starved
 * CI runner can take far longer than the local cadence suggests. */
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms, int step_ms = 10)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

/* Drain the surrogate ring via the client read API into a capture. */
void drain_sur_frames(qttune_j2534_sur_handle_t* h, SurCapture* cap)
{
    for (;;) {
        qttune_frame_t f = {};
        bool got = false;
        qttune_status_t st = qttune_j2534_sur_read_frame(h, 0, &f, 50, &got);
        if (st != QT_OK) return;
        if (!got) return;
        sur_capture_cb(&f, cap);
    }
}

/* Flattened-SCONFIG helper: one (Parameter, Value) u32 pair, host
 * endianness (memcpy) — valid because every participant in this
 * suite is x86 or x64, both little-endian, and both sides of the
 * wire use memcpy-based (un)flattening. */
void put_pair(unsigned char* buf, uint32_t param, uint32_t value)
{
    memcpy(buf + 0, &param, 4);
    memcpy(buf + 4, &value, 4);
}

uint32_t get_u32(const unsigned char* buf)
{
    uint32_t v = 0;
    memcpy(&v, buf, 4);
    return v;
}

} // namespace

TEST(J2534SurrogateTest, OpenConnectInjectReadCloseCycle)
{
    qttune_j2534_sur_handle_t* h = nullptr;

    /* Spawn: creates mapping/event/pipe, spawns under the kill-on-close
     * job, surrogate loads the fake DLL, calls PassThruOpen, handshakes. */
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    ASSERT_NE(h, nullptr);

    /* Connect with default sentinel baud — resolves to ISO 15765 500k
     * client-side per the loader contract; the surrogate relays the
     * resolved values verbatim to the fake's PassThruConnect. */
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* Inject THROUGH the surrogate (control pipe -> InjectFrame in the
     * surrogate's module instance -> fake FIFO). Varied flags exercise
     * the RxStatus -> QTTUNE_FRAME_FLAG_EXTENDED translation both ways;
     * timestamps are ours and must pass through untranslated. */
    const unsigned char f0[8] = {0x02, 0x01, 0x00, 0, 0, 0, 0, 0};
    const unsigned char f1[8] = {0x62, 0xF1, 0x90, 0x2A, 0, 0, 0, 0};
    const unsigned char f2[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f0, 8, 1000, 0), QT_OK);
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f1, 8, 2000, QTTUNE_PT_CAN_29BIT_ID), QT_OK);
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(
                  h, f2, 8, 3000, 0), QT_OK);

    SurCapture cap;
    /* The predicate MUST drain — cap only grows when reads are
     * issued over the pipe, so waiting on count without draining
     * inside the loop burns the full timeout every run. Sub-second
     * when IPC works. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 3; },
        10000, 50);
    ASSERT_TRUE(delivered);
    drain_sur_frames(h, &cap);

    /* EXACT count: the fake fabricates nothing we did not inject,
     * and the pipe drops nothing we did. */
    ASSERT_EQ(cap.count.load(), 3);

    ASSERT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[1], f1, 8), 0);
    ASSERT_EQ(memcmp(cap.payloads[2], f2, 8), 0);

    /* Flag translation through the whole IPC hop */
    EXPECT_EQ(cap.frame_flags[0], 0u);
    EXPECT_EQ(cap.frame_flags[1], QTTUNE_FRAME_FLAG_EXTENDED);
    EXPECT_EQ(cap.frame_flags[2], 0u);

    /* Vendor timestamp passthrough (documented v0.3.0 contract) */
    EXPECT_EQ(cap.timestamps[0], uint64_t{1000});
    EXPECT_EQ(cap.timestamps[1], uint64_t{2000});
    EXPECT_EQ(cap.timestamps[2], uint64_t{3000});

    /* Teardown: SHUTDOWN, bounded wait, job-object kill. */
    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, ReadPumpIsGatedOnConnect)
{
    /* Frame injected PRE-connect parks in the fake's FIFO; the pump
     * gate (g_channel == 0) must hold it there. It arrives only after
     * CONNECT — the fake's InjectFrame and ReadMsgs share one queue,
     * so parking is real, not an artifact of separate containers. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);

    SurCapture cap;
    const unsigned char f0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(h, f0, 8, 500, 0), QT_OK);

    /* Pre-connect: nothing may arrive. Predicate drains as it waits. */
    const bool early = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() > 0; },
        1500, 50);
    EXPECT_FALSE(early) << "frame flowed before CONNECT — pump gating broken";
    EXPECT_EQ(cap.count.load(), 0);

    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* Post-connect: the parked frame arrives. */
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(memcmp(cap.payloads[0], f0, 8), 0);
    EXPECT_EQ(cap.timestamps[0], uint64_t{500});

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, DeadDllFailsFastAtOpen)
{
    /* A surrogate spawned with an unloadable DLL must die before (or
     * during) the handshake; the client must notice via the process
     * handle wait, not by burning the 8s deadline. QT_ERR_LOAD_FAILED
     * is the contract. Wall-clock guard: 5s budget proves fail-fast. */
    const auto t0 = std::chrono::steady_clock::now();
    qttune_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(qttune_j2534_sur_open("C:\\does\\not\\exist.dll", &h),
              QT_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5000);
}

TEST(J2534SurrogateTest, NullArgumentsRejected)
{
    qttune_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(qttune_j2534_sur_open(nullptr, &h), QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_j2534_sur_open(FAKE_J2534_DLL_PATH, nullptr),
              QT_ERR_NULL_ARGUMENT);
    EXPECT_EQ(qttune_j2534_sur_close(nullptr), QT_OK);   /* free(nullptr) */
}

/* ================= WRITE / IOCTL / FILTER relay ================= */

TEST(J2534SurrogateTest, WriteRoundTripProducesEchoResponse)
{
    /* The full vendor request path minus a vehicle: WRITE crosses the
     * pipe, the surrogate materializes a PASSTHRU_MSG and calls
     * PassThruWriteMsgs, the fake records it and enqueues its
     * deterministic echo (byte 0 + 0x40), and our next OP_READ brings
     * it back over the pipe. Observable from EITHER architecture — no
     * fake-instance hooks involved. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* A realistic Mode-01 request shape (service 0x01, PID 0x0C RPM). */
    const unsigned char req[3] = {0x01, 0x0C, 0x00};
    const unsigned char expect[3] = {0x41, 0x0C, 0x00};   /* +0x40 */
    uint32_t num_written = 0;
    ASSERT_EQ(qttune_j2534_sur_write_frame(h, channel, req, 3, 0, 1000,
                                            &num_written),
              QT_OK);
    EXPECT_EQ(num_written, 1u);

    SurCapture cap;
    const bool delivered = wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        10000, 50);
    ASSERT_TRUE(delivered);
    ASSERT_EQ(cap.count.load(), 1);
    EXPECT_EQ(cap.payloads[0][0], expect[0]);
    EXPECT_EQ(memcmp(cap.payloads[0], expect, 3), 0);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, IoctlSetGetConfigRoundTrip)
{
    /* SET_CONFIG then GET_CONFIG across the flattened-SCONFIG wire:
     * the child materializes a real SCONFIG_LIST (pointer valid only
     * in ITS address space), stores, then GET must report the stored
     * value back through the re-flattened response payload. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    constexpr uint32_t kParam = 0x10;        /* arbitrary parameter id */
    constexpr uint32_t kValue = 0xBEEF;

    unsigned char in[8];
    put_pair(in, kParam, kValue);
    ASSERT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_SET_CONFIG,
                                     in, 8, nullptr, nullptr),
              QT_OK);

    unsigned char q[8], out[64];
    put_pair(q, kParam, 0);
    uint32_t out_len = 0;
    ASSERT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_GET_CONFIG,
                                     q, 8, out, &out_len),
              QT_OK);
    ASSERT_EQ(out_len, 8u);
    EXPECT_EQ(get_u32(out), kParam);
    EXPECT_EQ(get_u32(out + 4), kValue);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, IoctlReadVbattReturnsFixedVoltage)
{
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    unsigned char out[8] = {};
    uint32_t out_len = 0;
    ASSERT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_READ_VBATT,
                                     nullptr, 0, out, &out_len),
              QT_OK);
    ASSERT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);   /* 13.8 V in mV, per the fake */

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, IoctlWhitelistRefusesUnsupportedIds)
{
    /* Both ends must refuse an out-of-contract ioctl: the loader seam
     * returns NOT_IMPLEMENTED before the pipe; the CHILD whitelist
     * (patched into the OP_IOCTL handler) must independently fail it
     * if a request somehow arrives — vendor FAILED status crosses the
     * pipe as QT_ERR_NO_TRANSPORT. Driving the client directly (this
     * suite) exercises the child side. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* 0x05 = FIVE_BAUD_INIT — pointer-bearing, never shapable. */
    EXPECT_EQ(qttune_j2534_sur_ioctl(h, channel, 0x05,
                                     nullptr, 0, nullptr, nullptr),
              QT_ERR_NO_TRANSPORT);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

TEST(J2534SurrogateTest, FilterStartStopLifecycle)
{
    /* Register a PASS filter for a 29-bit-style 4-byte pattern, stop
     * it, then stop it AGAIN — the strict-vendor stop-twice refusal
     * must surface as an error, proving vendor status isn't being
     * flattened into silent success anywhere on the relay. */
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    const unsigned char mask[4]    = {0x00, 0x00, 0x07, 0xFF};
    const unsigned char pattern[4]  = {0x00, 0x00, 0x07, 0xE8};

    unsigned long filter_id = 0;
    ASSERT_EQ(qttune_j2534_sur_start_msg_filter(
                  h, channel, QTTUNE_PT_PASS_FILTER,
                  mask, 4, pattern, 4,
                  nullptr, 0, 0, &filter_id),
              QT_OK);
    EXPECT_GE(filter_id, 1ul);

    /* Malformed shapes are refused client-side before the pipe. */
    unsigned long junk = 0;
    EXPECT_EQ(qttune_j2534_sur_start_msg_filter(
                  h, channel, QTTUNE_PT_PASS_FILTER,
                  mask, 4, pattern, 3,     /* length mismatch */
                  nullptr, 0, 0, &junk),
              QT_ERR_NULL_ARGUMENT);
    /* Flow control on a non-FC filter is also refused. */
    EXPECT_EQ(qttune_j2534_sur_start_msg_filter(
                  h, channel, QTTUNE_PT_PASS_FILTER,
                  mask, 4, pattern, 4,
                  pattern, 4, 0, &junk),
              QT_ERR_NULL_ARGUMENT);

    EXPECT_EQ(qttune_j2534_sur_stop_msg_filter(h, channel, filter_id), QT_OK);
    EXPECT_EQ(qttune_j2534_sur_stop_msg_filter(h, channel, filter_id),
              QT_ERR_NO_TRANSPORT);   /* already stopped — strict vendor */

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

/* A mid-session vendor call that outlives the client's 3s transact
 * timeout leaves its response in flight. The NEXT request must
 * recover — consume the stale response — not desync against it.
 * Without that, this second, valid ioctl fails on a request-ID
 * mismatch. Knob armed BEFORE open (the child snapshots the environment at
 * spawn; SetEnvironmentVariableA afterwards does not propagate) and
 * restored before any later test spawns. */
TEST(J2534SurrogateTest, TimedOutOpDoesNotDesyncNextRequest)
{
    SetEnvironmentVariableA("QTTUNE_FAKE_IOCTL_DELAY_MS", "3500");
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    SetEnvironmentVariableA("QTTUNE_FAKE_IOCTL_DELAY_MS", nullptr);

    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    /* Vendor ioctl sleeps 3.5s — 0.5s past the 3s transact timeout:
     * parent gives up at ~3s, response lands at ~3.5s. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_READ_VBATT,
                                     nullptr, 0, out, &out_len),
              QT_ERR_NO_TRANSPORT);

    /* The differentiator: with the desync present this FAILS (reads
     * the stale response, ID mismatch); with recovery it succeeds. */
    out_len = 0;
    EXPECT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_READ_VBATT,
                                     nullptr, 0, out, &out_len),
              QT_OK);
    EXPECT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

/* A vendor PassThruOpen that blocks (adapter absent) must
 * fail at the handshake timeout with the child ALIVE, not wedge and
 * not require process-death to detect. The child inherits the parent's
 * environment, so arming the fake-side sleep from here works in the
 * cross-arch configuration too (x64 test -> x86 child). */
TEST(J2534SurrogateTest, BlockedVendorOpenFailsAtHandshakeTimeout)
{
    SetEnvironmentVariableA("QTTUNE_FAKE_OPEN_DELAY_MS", "60000");
    SetEnvironmentVariableA("QTTUNE_SUR_HANDSHAKE_MS", "500");
    const auto t0 = std::chrono::steady_clock::now();

    qttune_j2534_sur_handle_t* h = nullptr;
    const qttune_status_t rc = qttune_j2534_sur_open(fake_dll_path(), &h);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    /* Restore BEFORE assertions: a failing EXPECT would otherwise leak
     * the knobs into later tests in this process (sequential runs). */
    SetEnvironmentVariableA("QTTUNE_FAKE_OPEN_DELAY_MS", nullptr);
    SetEnvironmentVariableA("QTTUNE_SUR_HANDSHAKE_MS", nullptr);

    EXPECT_EQ(rc, QT_ERR_LOAD_FAILED);
    EXPECT_EQ(h, nullptr);
    /* Failed at the SHRUNK window (~0.5s), nowhere near the 8s default —
     * if this approaches 8s, an env knob stopped being honored. */
    EXPECT_LT(elapsed, 3000);
}

/* Shares read access with the session's writers. "" if it can't open. */
static std::string read_session_log(const std::string& path)
{
    std::string out;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(f);
    return out;
}

/* The client's own lines go into the session log beside the child's, so a GUI launch (no stderr) keeps
 * both halves in one file. Each side writes whole lines, so every line
 * starts "[sur" ("[sur ..." from the client, "[surrogate ..." from the
 * child). */
TEST(J2534SurrogateTest, ClientAndChildLinesShareTheSessionLog)
{
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    const std::string path = qttune_j2534_sur_log_path(h);
    /* Close first: the child exits, so the file is complete. */
    ASSERT_EQ(qttune_j2534_sur_close(h), QT_OK);
    ASSERT_FALSE(path.empty()) << "the session ran without a log";

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("[sur "), std::string::npos) << log;
    EXPECT_NE(log.find("open: child pid"), std::string::npos) << log;
    EXPECT_NE(log.find("[surrogate "), std::string::npos) << log;
    EXPECT_NE(log.find("pipe connected"), std::string::npos) << log;

    int lines = 0;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t eol = log.find('\n', pos);
        if (eol == std::string::npos) eol = log.size();
        std::string line = log.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            ++lines;
            EXPECT_EQ(line.rfind("[sur", 0), 0u) << "spliced line: " << line;
        }
        pos = eol + 1;
    }
    EXPECT_GE(lines, 4);   /* client spawn line + child startup stages */
}

namespace {
/* Sets an environment variable for one scope. The child snapshots its
 * environment at spawn, so arm before open; the destructor clears the
 * variable even when an ASSERT returns early. */
struct EnvArm {
    const char* name;
    EnvArm(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
    ~EnvArm() { SetEnvironmentVariableA(name, nullptr); }
    EnvArm(const EnvArm&) = delete;
    EnvArm& operator=(const EnvArm&) = delete;
};
}  // namespace

/* QTTUNE_SUR_LOG_FRAMES=1 puts every message written and read into the
 * session log verbatim: status, flags, the vendor timestamp and the
 * bytes. Without it the log keeps its counts and status changes only. */
TEST(J2534SurrogateTest, FrameLoggingRecordsPayloadsVerbatimWhenEnabled)
{
    const uint8_t req[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    const uint8_t rsp[8] = {0x00, 0x00, 0x07, 0xE8, 0x41, 0x0C, 0x1F, 0x40};
    std::string logs[2];   /* [0] disabled, [1] enabled */

    for (int enabled = 1; enabled >= 0; --enabled) {
        EnvArm arm("QTTUNE_SUR_LOG_FRAMES", enabled ? "1" : nullptr);
        qttune_j2534_sur_handle_t* h = nullptr;
        ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
        unsigned long channel = 0;
        ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

        uint32_t written = 0;
        ASSERT_EQ(qttune_j2534_sur_write_frame(h, channel, req, sizeof(req),
                                               QTTUNE_PT_TX_ISO15765_FRAME_PAD,
                                               0, &written), QT_OK);
        ASSERT_EQ(qttune_j2534_sur_inject_test_frame(h, rsp, sizeof(rsp),
                                                     4242, 0), QT_OK);
        /* The fake returns one message per read: the write's echo, then
         * the injected frame. Extra reads come back empty. */
        for (int i = 0; i < 4; ++i) {
            qttune_frame_t f = {};
            bool got = false;
            (void)qttune_j2534_sur_read_frame(h, channel, &f, 20, &got);
        }

        const std::string path = qttune_j2534_sur_log_path(h);
        ASSERT_EQ(qttune_j2534_sur_close(h), QT_OK);   /* child exits: log complete */
        ASSERT_FALSE(path.empty());
        logs[enabled] = read_session_log(path);
    }

    const std::string& on = logs[1];
    EXPECT_NE(on.find("frame logging on (QTTUNE_SUR_LOG_FRAMES=1)"), std::string::npos) << on;
    EXPECT_NE(on.find("OP_WRITE frame: status 0, tx 0x00000040, len 6: 00 00 07 E0 01 0C"),
              std::string::npos) << on;
    EXPECT_NE(on.find("ts 4242, len 8: 00 00 07 E8 41 0C 1F 40"), std::string::npos) << on;

    const std::string& off = logs[0];
    EXPECT_EQ(off.find(" frame: "), std::string::npos) << "frame lines without the knob:\n" << off;
    EXPECT_NE(off.find("OP_WRITE: status 0"), std::string::npos) << off;
}

/* The session log is opened CREATE_ALWAYS with FILE_SHARE_READ only,
 * so while one session's child holds it, a second concurrent
 * session's open fails. That session must get a per-spawn name, not
 * run its child with no log at all. Holds
 * whether or not something else (a running app) already has the fixed
 * name: then both sessions get per-spawn names. */
TEST(J2534SurrogateTest, SecondConcurrentSessionGetsItsOwnLog)
{
    qttune_j2534_sur_handle_t* a = nullptr;
    qttune_j2534_sur_handle_t* b = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &a), QT_OK);
    const qttune_status_t rb = qttune_j2534_sur_open(fake_dll_path(), &b);
    const std::string path_a = qttune_j2534_sur_log_path(a);
    const std::string path_b = qttune_j2534_sur_log_path(b);
    qttune_j2534_sur_close(b);
    qttune_j2534_sur_close(a);

    ASSERT_EQ(rb, QT_OK);
    EXPECT_FALSE(path_a.empty());
    EXPECT_FALSE(path_b.empty()) << "the second session ran without a log";
    EXPECT_NE(path_a, path_b);
    EXPECT_NE(read_session_log(path_b).find("pipe connected"),
              std::string::npos);

    /* Per-spawn names are this test's litter; the fixed name stays. */
    for (const std::string* p : { &path_a, &path_b }) {
        if (p->find("qttune-surrogate-") != std::string::npos) {
            DeleteFileA(p->c_str());
        }
    }
}

/* Per J2534-1, a read asking for more messages than arrived, with a
 * nonzero timeout, returns ERR_TIMEOUT TOGETHER WITH those messages.
 * The spec-mode fake emulates exactly that; the client must deliver
 * the batch and only judge the status when the batch is empty. A
 * client that judged the status first would discard the batch and
 * render a talking bus invisible. Knob armed BEFORE
 * open — the child snapshots the environment at spawn (same
 * discipline as QTTUNE_FAKE_IOCTL_DELAY_MS). */
TEST(J2534SurrogateTest, SpecTimeoutReadDeliversPartialBatch)
{
    SetEnvironmentVariableA("QTTUNE_FAKE_SPEC_READ_SEMANTICS", "1");
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    const unsigned char f0[8] = {0x41, 0x0C, 0x1F, 0x40, 0, 0, 0, 0};
    ASSERT_EQ(qttune_j2534_sur_inject_test_frame(h, f0, 8, 4242, 0),
              QT_OK);

    qttune_frame_t f1 = {};
    bool got1 = false;
    const qttune_status_t st1 =
        qttune_j2534_sur_read_frame(h, 0, &f1, 50, &got1);

    qttune_frame_t f2 = {};
    bool got2 = false;
    const qttune_status_t st2 =
        qttune_j2534_sur_read_frame(h, 0, &f2, 50, &got2);

    /* Restore BEFORE assertions (knob-leak discipline) */
    SetEnvironmentVariableA("QTTUNE_FAKE_SPEC_READ_SEMANTICS", nullptr);

    /* Child asks for 16 with a 50 ms timeout; one queued -> spec mode
     * returns 1 message WITH ERR_TIMEOUT. The client must deliver it. */
    EXPECT_EQ(st1, QT_OK);
    EXPECT_TRUE(got1);
    EXPECT_EQ(memcmp(f1.data, f0, 8), 0);
    EXPECT_EQ(f1.data_length, 8u);
    EXPECT_EQ(f1.timestamp_us, uint64_t{4242});

    /* Empty queue, nonzero timeout: quiet, not delivered, not hard. */
    EXPECT_EQ(st2, QT_ERR_VEHICLE_TIMEOUT);
    EXPECT_FALSE(got2);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

/* Count-trust rule, surrogate half: a vendor returning a hard error
 * without touching pNumMsgs leaves the caller's count dirty. Without
 * qttune_trusted_count the child's static read batch would deliver up to
 * 16 stale/zero frames and the client reports the batch as healthy.
 * The knob is sticky in the child (env inherited at spawn; the
 * parent's clear never propagates), so recovery is not observable
 * here — instead the follow-up ioctl proves request correlation
 * survived the poisoned request. */
TEST(J2534SurrogateTest, HardErrorUntouchedCountDeliversNoFrames)
{
    SetEnvironmentVariableA("QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS", "7");
    qttune_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(qttune_j2534_sur_open(fake_dll_path(), &h), QT_OK);
    SetEnvironmentVariableA("QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS", nullptr);
    unsigned long channel = 0;
    ASSERT_EQ(qttune_j2534_sur_connect_channel(h, 0, 0, &channel), QT_OK);

    qttune_frame_t f = {};
    bool got = false;
    const qttune_status_t st =
        qttune_j2534_sur_read_frame(h, 0, &f, 50, &got);

    EXPECT_EQ(st, QT_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);

    /* No desync and a live channel after the poisoned request. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(qttune_j2534_sur_ioctl(h, channel, QTTUNE_PT_IOCTL_READ_VBATT,
                                     nullptr, 0, out, &out_len),
              QT_OK);
    EXPECT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);

    EXPECT_EQ(qttune_j2534_sur_close(h), QT_OK);
}

#endif /* _WIN32 */
