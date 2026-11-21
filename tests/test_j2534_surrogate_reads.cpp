// File/layer: tests/test_j2534_surrogate_reads.cpp | GTest, HARNESS tier
// Testing: HARNESS: J2534SurrogateTest (test_j2534_surrogate)

#include "test_j2534_surrogate_support.h"

#ifdef _WIN32

namespace {

std::wstring to_wide(const std::string& utf8)
{
    wchar_t buf[MAX_PATH + 64] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, MAX_PATH + 64);
    return buf;
}

std::string to_utf8(const std::wstring& wide)
{
    char buf[3 * (MAX_PATH + 64)] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buf, sizeof(buf),
                        nullptr, nullptr);
    return buf;
}

} // namespace

/* Each session takes the fixed log name, and the log it replaces is
 * kept as ecuio-surrogate.<last write>.log, so a run of connects keeps
 * every session's log. Skips when something else (a running app, a
 * parallel test) holds the fixed name: sessions then get per-spawn
 * names and nothing is replaced. */
TEST(J2534SurrogateTest, EarlierSessionLogIsKept)
{
    const std::string fixed_name = "ecuio-surrogate.log";
    ecuio_j2534_sur_handle_t* a = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &a), ECUIO_OK);
    const std::string path_a = ecuio_j2534_sur_log_path(a);
    ASSERT_EQ(ecuio_j2534_sur_close(a), ECUIO_OK);
    if (path_a.size() < fixed_name.size() ||
        path_a.compare(path_a.size() - fixed_name.size(), fixed_name.size(),
                       fixed_name) != 0) {
        DeleteFileW(to_wide(path_a).c_str());   /* a per-spawn name: litter */
        GTEST_SKIP() << "the fixed log name is held elsewhere";
    }
    const std::string log_a = read_session_log(path_a);
    ASSERT_NE(log_a.find("open: child pid"), std::string::npos) << log_a;

    ecuio_j2534_sur_handle_t* b = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &b), ECUIO_OK);
    const std::string path_b = ecuio_j2534_sur_log_path(b);
    ASSERT_EQ(ecuio_j2534_sur_close(b), ECUIO_OK);
    EXPECT_EQ(path_b, path_a) << "the second session should take the fixed name";
    EXPECT_NE(read_session_log(path_b), log_a);

    /* The first session's log, byte for byte, under a kept name. */
    const std::wstring dir = to_wide(path_a.substr(0, path_a.size() - fixed_name.size()));
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"ecuio-surrogate.*.log").c_str(), &fd);
    ASSERT_NE(find, INVALID_HANDLE_VALUE) << "no kept log";
    std::wstring kept;
    do {
        if (read_session_log(to_utf8(dir + fd.cFileName)) == log_a) {
            kept = fd.cFileName;
        }
    } while (kept.empty() && FindNextFileW(find, &fd));
    FindClose(find);
    ASSERT_FALSE(kept.empty()) << "the first session's log wasn't kept";
    /* "ecuio-surrogate." + "YYYYMMDD-HHMMSS-mmm" + ".log" */
    EXPECT_EQ(kept.size(), 16u + 19u + 4u) << to_utf8(kept);
}

/* Per J2534-1, a read asking for more messages than arrived, with a
 * nonzero timeout, returns ERR_TIMEOUT TOGETHER WITH those messages.
 * The spec-mode fake emulates exactly that. The child's first call
 * asks for one message, so it returns as soon as one has arrived, and
 * its call for the rest of the batch is such a short read: the second
 * message comes back under ERR_TIMEOUT and must be delivered, from the
 * same response. An empty read holds its timeout and is quiet. Knob
 * armed BEFORE open: the child snapshots the environment at spawn (same
 * discipline as ECUIO_FAKE_IOCTL_DELAY_MS). */
TEST(J2534SurrogateTest, SpecTimeoutReadDeliversPartialBatch)
{
    SetEnvironmentVariableA("ECUIO_FAKE_SPEC_READ_SEMANTICS", "1");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    const unsigned char f0[8] = {0x41, 0x0C, 0x1F, 0x40, 0, 0, 0, 0};
    const unsigned char f0b[8] = {0x41, 0x0D, 0x20, 0, 0, 0, 0, 0};
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, f0, 8, 4242, 0),
              ECUIO_OK);
    ASSERT_EQ(ecuio_j2534_sur_inject_test_frame(h, f0b, 8, 4243, 0), ECUIO_OK);

    const auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t).count();
    };
    ecuio_pt_rx_t f1 = {};
    bool got1 = false;
    auto t = std::chrono::steady_clock::now();
    const ecuio_status_t st1 =
        ecuio_j2534_sur_read_msg(h, 0, &f1, 50, &got1);
    const auto held1 = ms_since(t);

    /* The rest of the batch, from the client's cache */
    ecuio_pt_rx_t f2 = {};
    bool got2 = false;
    const ecuio_status_t st2 =
        ecuio_j2534_sur_read_msg(h, 0, &f2, 50, &got2);

    ecuio_pt_rx_t f3 = {};
    bool got3 = false;
    t = std::chrono::steady_clock::now();
    const ecuio_status_t st3 = ecuio_j2534_sur_read_msg(h, 0, &f3, 50, &got3);
    const auto held3 = ms_since(t);

    /* Restore BEFORE assertions (knob-leak discipline) */
    SetEnvironmentVariableA("ECUIO_FAKE_SPEC_READ_SEMANTICS", nullptr);

    EXPECT_EQ(st1, ECUIO_OK);
    EXPECT_TRUE(got1);
    EXPECT_EQ(memcmp(f1.data, f0, 8), 0);
    EXPECT_EQ(f1.data_size, 8u);
    EXPECT_EQ(f1.timestamp, 4242u);
    EXPECT_LT(held1, 45) << "the read held its timeout past its message";

    /* The child's second call asked for 15 with 1 ms; one queued: spec
     * mode returned it WITH ERR_TIMEOUT, and it was delivered */
    EXPECT_EQ(st2, ECUIO_OK);
    EXPECT_TRUE(got2) << "the short batch's message was dropped";
    EXPECT_EQ(memcmp(f2.data, f0b, 8), 0);
    EXPECT_EQ(f2.timestamp, 4243u);

    /* Empty queue, nonzero timeout: quiet, not delivered, not hard. */
    EXPECT_EQ(st3, ECUIO_ERR_VEHICLE_TIMEOUT);
    EXPECT_FALSE(got3);
    EXPECT_GE(held3, 45) << "the empty read came back before its timeout";

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

/* A read's reply carries its message whole, however long: sixteen of
 * the fake's 0xF1A1 replies, each ISO 15765-2's longest message with
 * its CAN ID in front (4099 bytes), queued at once (the fake's spec mode
 * batches), come back one a read, every byte as the vendor gave it: the
 * child answers a read with its first message. */
TEST(J2534SurrogateTest, LongMessagesCrossThePipeWholeOneARead)
{
    EnvArm spec("ECUIO_FAKE_SPEC_READ_SEMANTICS", "1");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char pattern[4] = {0x00, 0x00, 0x07, 0xE8};
    unsigned long filter_id = 0;
    ASSERT_EQ(ecuio_j2534_sur_start_msg_filter(
                  h, channel, ECUIO_PT_PASS_FILTER, mask, 4, pattern, 4,
                  nullptr, 0, 0, &filter_id),
              ECUIO_OK);
    const std::string path = ecuio_j2534_sur_log_path(h);

    const unsigned char req[7] = {0x00, 0x00, 0x07, 0xE0, 0x22, 0xF1, 0xA1};
    constexpr int kMessages = 16;
    for (int i = 0; i < kMessages; ++i) {
        uint32_t written = 0;
        ASSERT_EQ(ecuio_j2534_sur_write_frame(h, channel, req, sizeof(req),
                                              ECUIO_PT_TX_ISO15765_FRAME_PAD,
                                              0, &written), ECUIO_OK) << i;
    }
    int whole = 0;
    for (int i = 0; i < kMessages; ++i) {
        ecuio_pt_rx_t m = {};
        bool got = false;
        ASSERT_EQ(ecuio_j2534_sur_read_msg(h, channel, &m, 50, &got),
                  ECUIO_OK) << i;
        ASSERT_TRUE(got) << i;
        bool same = m.data_size == 4099u && m.data[3] == 0xE8 &&
                    m.data[4] == 0x62 && m.data[6] == 0xA1;
        for (uint32_t b = 7; same && b < m.data_size; ++b) {
            same = m.data[b] == static_cast<uint8_t>(b - 7);
        }
        whole += same ? 1 : 0;
    }
    EXPECT_EQ(whole, kMessages);
    ASSERT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);   /* child exits: log complete */
    const std::string log = read_session_log(path);
    size_t reads = 0;
    for (size_t at = log.find("OP_READ: status 0, frames ");
         at != std::string::npos;
         at = log.find("OP_READ: status 0, frames ", at + 1)) {
        ++reads;
        const size_t n = at + strlen("OP_READ: status 0, frames ");
        EXPECT_EQ(log.compare(n, 2, "1\r") == 0 ||
                      log.compare(n, 2, "1\n") == 0,
                  true)
            << "a read carried more than one message:\n"
            << log;
    }
    EXPECT_EQ(reads, static_cast<size_t>(kMessages));
}

/* A request to 0x7E0 on a channel filtered for 0x7E8, and the reply
 * after its 4-byte ID ({} when none came within a second) */
static std::vector<uint8_t> ask_ecu(ecuio_j2534_sur_handle_t* h,
                                    unsigned long channel,
                                    std::vector<uint8_t> req)
{
    req.insert(req.begin(), {0x00, 0x00, 0x07, 0xE0});
    uint32_t written = 0;
    EXPECT_EQ(ecuio_j2534_sur_write_frame(
                  h, channel, req.data(), static_cast<uint32_t>(req.size()),
                  ECUIO_PT_TX_ISO15765_FRAME_PAD, 0, &written),
              ECUIO_OK);
    ecuio_pt_rx_t m = {};
    bool got = false;
    for (int i = 0; i < 20 && !got; ++i) {
        (void)ecuio_j2534_sur_read_msg(h, channel, &m, 50, &got);
    }
    if (!got || m.data_size < 4) {
        return {};
    }
    return std::vector<uint8_t>(m.data + 4, m.data + m.data_size);
}

/* The fakes' ECU's diagnostics through the fake DLL in the surrogate
 * (fake_ecu.h; the adapter's side, so no ISO-TP framing): Service 03's
 * list, a clear refused under ECUIO_FAKE_CLEAR_NRC, then one taken in a
 * fresh child, whose diagnostics start uncleared (one state per
 * process) */
TEST(J2534SurrogateTest, TheFakeEcuAnswersTheDiagnosticServices)
{
    const unsigned char mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char pattern[4] = {0x00, 0x00, 0x07, 0xE8};
    const std::vector<uint8_t> stored = {0x43, 0x03, 0x03, 0x01,
                                         0x04, 0x20, 0xC1, 0x00};
    for (const bool refused : {true, false}) {
        SCOPED_TRACE(refused ? "ECUIO_FAKE_CLEAR_NRC=0x22" : "no knob");
        EnvArm nrc("ECUIO_FAKE_CLEAR_NRC", refused ? "0x22" : nullptr);
        ecuio_j2534_sur_handle_t* h = nullptr;
        ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h),
                  ECUIO_OK);
        unsigned long channel = 0;
        ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);
        unsigned long filter_id = 0;
        ASSERT_EQ(ecuio_j2534_sur_start_msg_filter(
                      h, channel, ECUIO_PT_PASS_FILTER, mask, 4, pattern, 4,
                      nullptr, 0, 0, &filter_id),
                  ECUIO_OK);

        EXPECT_EQ(ask_ecu(h, channel, {0x03}), stored);
        if (refused) {
            EXPECT_EQ(ask_ecu(h, channel, {0x04}),
                      (std::vector<uint8_t>{0x7F, 0x04, 0x22}));
            EXPECT_EQ(ask_ecu(h, channel, {0x03}), stored);
        } else {
            EXPECT_EQ(ask_ecu(h, channel, {0x04}),
                      (std::vector<uint8_t>{0x44}));
            EXPECT_EQ(ask_ecu(h, channel, {0x03}),
                      (std::vector<uint8_t>{0x43, 0x00}));
            EXPECT_EQ(ask_ecu(h, channel, {0x02, 0x02, 0x00}),
                      (std::vector<uint8_t>{0x42, 0x02, 0x00, 0x00, 0x00}));
        }
        EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
    }
}

/* Count-trust rule, surrogate half: a vendor returning a hard error
 * without touching pNumMsgs leaves the caller's count dirty. Without
 * ecuio_trusted_count the child's static read batch would deliver up to
 * 16 stale/zero frames and the client reports the batch as healthy.
 * The knob is sticky in the child (env inherited at spawn; the
 * parent's clear never propagates), so recovery is not observable
 * here: instead the follow-up ioctl proves request correlation
 * survived the poisoned request. */
TEST(J2534SurrogateTest, HardErrorUntouchedCountDeliversNoFrames)
{
    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", "7");
    ecuio_j2534_sur_handle_t* h = nullptr;
    ASSERT_EQ(ecuio_j2534_sur_open(fake_dll_path(), fake_dll_arch(), &h), ECUIO_OK);
    SetEnvironmentVariableA("ECUIO_FAKE_UNTOUCHED_COUNT_STATUS", nullptr);
    unsigned long channel = 0;
    ASSERT_EQ(ecuio_j2534_sur_connect_channel(h, 0, 0, &channel), ECUIO_OK);

    ecuio_pt_rx_t f = {};
    bool got = false;
    const ecuio_status_t st =
        ecuio_j2534_sur_read_msg(h, 0, &f, 50, &got);

    EXPECT_EQ(st, ECUIO_ERR_TRANSPORT_HARD_FAILURE);
    EXPECT_FALSE(got);

    /* No desync and a live channel after the poisoned request. */
    unsigned char out[8] = {};
    uint32_t out_len = 0;
    EXPECT_EQ(ecuio_j2534_sur_ioctl(h, channel, ECUIO_PT_IOCTL_READ_VBATT,
                                    nullptr, 0, out, sizeof(out), &out_len),
              ECUIO_OK);
    EXPECT_EQ(out_len, 4u);
    EXPECT_EQ(get_u32(out), 13800u);

    EXPECT_EQ(ecuio_j2534_sur_close(h), ECUIO_OK);
}

#endif // _WIN32
