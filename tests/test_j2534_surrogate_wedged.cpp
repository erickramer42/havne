// File/layer: tests/test_j2534_surrogate_wedged.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534SurrogateTest (test_j2534_surrogate)

#include "test_j2534_surrogate_support.h"

#ifdef _WIN32

/* The deadline as shipped; the case below shortens it */
TEST(J2534SurrogateTest, TheTransactionDeadlineIsThreeSeconds)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_get_transact_ms(h), 3000u);
    ecuio_j2534_sur_set_transact_ms(h, 200);
    EXPECT_EQ(ecuio_j2534_sur_get_transact_ms(h), 200u);
    ecuio_j2534_sur_set_transact_ms(h, 0);
    EXPECT_EQ(ecuio_j2534_sur_get_transact_ms(h), 3000u);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* A child that misses two deadlines in a row (a transaction's, then the
 * wait for its late reply) is wedged, not slow. Later transactions fail
 * at once instead of each costing another 3 s, which a teardown would
 * otherwise pay per filter, for the disconnect and for the close, and
 * close kills the child without waiting for a polite exit. A slow vendor
 * that answers inside the second wait still recovers
 * (TimedOutOpDoesNotDesyncNextRequest). The fake's one-shot ioctl stall
 * wedges the single-threaded child for 20 s, so both
 * deadlines here are missed whatever their length. */
TEST(J2534SurrogateTest, UnresponsiveChildFailsFastAfterTwoMissedDeadlines)
{
    using clock = std::chrono::steady_clock;
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        EnvArm arm("ECUIO_FAKE_IOCTL_DELAY_MS", "20000");
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);
    ecuio_j2534_sur_set_transact_ms(h, 200);

    /* First deadline: the ioctl outlives the transaction timeout. */
    const auto missed_from = clock::now();
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED);

    /* Second: its late reply doesn't come within another timeout. */
    EXPECT_EQ(ecuio_j2534_sur_disconnect_channel(h, channel),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - missed_from, std::chrono::milliseconds(4000))
        << "the handle's shortened deadline";

    /* From here, every transaction fails at once. */
    auto t0 = clock::now();
    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, channel, 1),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(500));

    /* Close kills the child instead of waiting 1.5 s twice, and says
     * it didn't shut down cleanly */
    t0 = clock::now();
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(1000));

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("child unresponsive"), std::string::npos) << log;
    EXPECT_NE(log.find("close: the child didn't shut down cleanly "
                       "(unresponsive, killed)"),
              std::string::npos)
        << log;
}

/* A child that exits on its own (a crash, or a child loop that ends on
 * a vendor status, which ADR-0014 forbids) fails the close: every test
 * that ends with sur_close == ECUIO_OK thereby asserts its child lived
 * through the refusals before it. The child is killed from outside,
 * found by the PID the fake records at PassThruOpen. */
TEST(J2534SurrogateTest, ACloseFailsWhenTheChildExitedOnItsOwn)
{
    fake_control::Page* page = fake_control::create_for_this_process();
    ASSERT_NE(page, nullptr);
    fake_control::reset(page);
    struct PageReset {
        fake_control::Page* p;
        ~PageReset() { fake_control::reset(p); }
    } page_reset{page};

    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);
    const DWORD pid = fake_control::opener_pid(page);
    ASSERT_NE(pid, 0u) << "the fake's PassThruOpen didn't record its process";
    ASSERT_NE(pid, GetCurrentProcessId())
        << "the fake ran in the test, not the child";

    HANDLE child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(TerminateProcess(child, 9));
    EXPECT_EQ(WaitForSingleObject(child, 5000), WAIT_OBJECT_0);
    CloseHandle(child);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_ERR_SURROGATE_FAILED);
    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("close: the child had already exited (code 9)"),
              std::string::npos)
        << log;
}

/* A reply the client never asked for (the child answers OP_CONNECT
 * twice, ECUIO_SUR_TEST_REPLY_TWICE) leaves the pipe one reply behind:
 * the next transaction reads the reply before its own. That's the pipe
 * out of step for good, so the handle turns unresponsive: later calls
 * fail at once without reaching the child, and close kills it. Treated
 * as a one-off, every later call would still be sent and fail on the
 * reply before its own. */
TEST(J2534SurrogateTest, RequestIdMismatchMakesTheChildUnresponsive)
{
    using clock = std::chrono::steady_clock;
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        char twice[8];
        snprintf(twice, sizeof(twice), "%u",
                 static_cast<unsigned>(ECUIO_SUR_OP_CONNECT));
        EnvArm arm("ECUIO_SUR_TEST_REPLY_TWICE", twice);
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED) << "read the connect's second reply";

    const auto t0 = clock::now();
    const uint8_t msg[6] = {0x00, 0x00, 0x07, 0xE0, 0x01, 0x0C};
    uint32_t written = 0;
    EXPECT_EQ(ecuio_j2534_sur_write_frame(h, channel, msg, sizeof(msg), 0, 0,
                                          &written),
              ECUIO_ERR_SURROGATE_FAILED);

    /* So does every other call, the connect-path ones with a detail. */
    unsigned long again = 0;
    EXPECT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &again),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate stopped answering during PassThruConnect");
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char id[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter = 0;
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(h, channel, ECUIO_PT_PASS_FILTER,
                                               mask, 4, id, 4, nullptr, 0, 0,
                                               &filter),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the surrogate stopped answering during "
                 "PassThruStartMsgFilter");
    char fw[ECUIO_SUR_TEXT_MAX], dll[ECUIO_SUR_TEXT_MAX],
        api[ECUIO_SUR_TEXT_MAX];
    EXPECT_EQ(ecuio_j2534_sur_read_version(h, fw, dll, api),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(ecuio_j2534_sur_inject_test_frame(h, msg, sizeof(msg), 0, 0),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_LT(clock::now() - t0, std::chrono::milliseconds(500));
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_ERR_SURROGATE_FAILED)
        << "a child out of step is killed, not shut down";

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("DESYNC"), std::string::npos) << log;
    EXPECT_NE(log.find("child unresponsive"), std::string::npos) << log;
    EXPECT_EQ(log.find("OP_WRITE:"), std::string::npos)
        << "a write reached the child after the mismatch\n" << log;
}

/* A child built against other wire limits (ECUIO_SUR_TEST_SKEW stands in
 * for one) can claim more than the client has room for. The client
 * refuses the reply before reading past it: a READ claiming more frames
 * than a batch holds, or frames on a reply that takes none, leaves the
 * pipe out of step, so the handle turns unresponsive and close kills the
 * child; an ioctl reply whose data_len is past the wire's limit is
 * refused alone, the pipe still in step. */
static ecuio_j2534_sur_handle_t* open_skewed(uint32_t opcode)
{
    char op[8];
    snprintf(op, sizeof(op), "%u", static_cast<unsigned>(opcode));
    EnvArm arm("ECUIO_SUR_TEST_SKEW", op);
    ecuio_j2534_sur_handle_t* h = nullptr;
    EXPECT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
              ECUIO_OK);
    return h;
}

TEST(J2534SurrogateTest, AReadClaimingMoreFramesThanABatchHoldsEndsTheHandle)
{
    ecuio_j2534_sur_handle_t* h = open_skewed(ECUIO_SUR_OP_READ);
    ASSERT_NE(h, nullptr);
    const std::string path = ecuio_j2534_sur_log_path(h);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    ecuio_pt_rx_t f = {};
    bool got = true;
    EXPECT_EQ(ecuio_j2534_sur_read_msg(h, channel, &f, 50, &got),
              ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED)
        << "the pipe is out of step: no later call reaches the child";
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_ERR_SURROGATE_FAILED);

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find(std::to_string(ECUIO_SUR_MAX_FRAMES_PER_READ + 1) +
                       " frame(s) on the reply to op=5 (skew?); child "
                       "unresponsive"),
              std::string::npos)
        << log;
}

TEST(J2534SurrogateTest, FramesOnAReplyThatTakesNoneEndTheHandle)
{
    ecuio_j2534_sur_handle_t* h = open_skewed(ECUIO_SUR_OP_CONNECT);
    ASSERT_NE(h, nullptr);
    const std::string path = ecuio_j2534_sur_log_path(h);
    unsigned long channel = 0;
    EXPECT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_ERR_SURROGATE_FAILED);

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("1 frame(s) on the reply to op=2 (skew?); child "
                       "unresponsive"),
              std::string::npos)
        << log;
}

TEST(J2534SurrogateTest, AnIoctlReplyPastTheWireLimitIsRefusedAlone)
{
    ecuio_j2534_sur_handle_t* h = open_skewed(ECUIO_SUR_OP_IOCTL);
    ASSERT_NE(h, nullptr);
    const std::string path = ecuio_j2534_sur_log_path(h);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    unsigned char out[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint32_t out_len = 1234;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_ERR_SURROGATE_FAILED);
    EXPECT_EQ(out_len, 0u) << "a refused reply reports no length";
    EXPECT_EQ(out[0], 0xAA) << "a refused reply copies nothing";

    /* Only the one reply was skewed, and the pipe is in step: the next
     * ioctl is the fake's 13.8 V, and the child shuts down cleanly */
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);

    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("ioctl: data_len=65 exceeds the wire limit (skew?)"),
              std::string::npos)
        << log;
}

/* A read the child answers past its deadline: the late reply, which
 * carries a frame, is drained and dropped before the next request, the
 * drop is logged with its count, and the next read is paired with its
 * own reply. The fake's one-shot read stall holds the first ReadMsgs
 * 1.5 s, past the 50 ms read's 550 ms deadline and well inside the
 * next transaction's 3 s. */
TEST(J2534SurrogateTest, LateReadReplyIsDroppedAndLogged)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        EnvArm arm("ECUIO_FAKE_READ_DELAY_MS", "1500");
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    const unsigned char late[8] = {0xA1, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, late, 8, 100, 0), ECUIO_OK);
    ecuio_pt_rx_t f = {};
    bool got = false;
    EXPECT_EQ(ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got),
              ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);

    /* This transaction drains the late reply first. */
    const unsigned char next[8] = {0xB2, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, next, 8, 200, 0), ECUIO_OK);
    SurCapture cap;
    ASSERT_TRUE(wait_until(
        [&]() { drain_sur_frames(h, &cap); return cap.count.load() >= 1; },
        5000, 50));
    EXPECT_EQ(cap.count.load(), 1);
    EXPECT_EQ(memcmp(cap.payloads[0], next, 8), 0)
        << "the late reply's frame was delivered";

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    const std::string log = read_session_log(path);
    EXPECT_NE(log.find("drain_pending: dropped 1 frame(s) from the late reply"),
              std::string::npos) << log;
}

/* The session log is opened CREATE_ALWAYS with FILE_SHARE_READ only,
 * so while one session's child holds it, a second concurrent
 * session's open fails. That session must get a per-spawn name, not
 * run its child with no log at all. Holds
 * whether or not something else (a running app) already has the fixed
 * name: then both sessions get per-spawn names. */
TEST(J2534SurrogateTest, SecondConcurrentSessionGetsItsOwnLog)
{
    ecuio_j2534_sur_handle_t* a = nullptr;
    ecuio_j2534_sur_handle_t* b = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &a), ECUIO_OK);
    const ecuio_status_t rb = ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &b);
    const std::string path_a = ecuio_j2534_sur_log_path(a);
    const std::string path_b = ecuio_j2534_sur_log_path(b);
    ecuio_j2534_sur_close(b);
    ecuio_j2534_sur_close(a);

    ASSERT_EQ(rb, ECUIO_OK);
    EXPECT_FALSE(path_a.empty());
    EXPECT_FALSE(path_b.empty()) << "the second session ran without a log";
    EXPECT_NE(path_a, path_b);
    EXPECT_NE(read_session_log(path_b).find("pipe connected"),
              std::string::npos);

    /* Per-spawn names are this test's litter; the fixed name stays. */
    for (const std::string* p : { &path_a, &path_b }) {
        if (p->find("ecuio-surrogate-") != std::string::npos) {
            DeleteFileA(p->c_str());
        }
    }
}

/* The knob splits the first request (OP_OPEN): the child must assemble it, not end */
TEST(J2534SurrogateTest, ASplitRequestIsAssembledNotTakenForAClosedPipe)
{
    ecuio_j2534_sur_handle_t* h = nullptr;
    {
        EnvArm arm("ECUIO_SUR_TEST_SPLIT_REQUEST", "8");
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
                  ECUIO_OK);
    }
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    unsigned char mask[4] = {};
    unsigned char pattern[4] = {};
    unsigned long filter_id = 0;
    EXPECT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER, mask, sizeof(mask), pattern,
                  sizeof(pattern), nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_stop_msg_filter(h, channel, filter_id), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_disconnect_channel(h, channel), ECUIO_OK);
    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

#endif // _WIN32
