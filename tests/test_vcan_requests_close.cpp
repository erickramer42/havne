// File/layer: tests/test_vcan_requests_close.cpp | test-only, C++17, Qt-free, Linux only
// Testing: HARNESS: SocketCanFaultTest, SocketCanRequestTest (test_vcan)

#include "test_vcan_requests_support.h"

namespace {

/* The ECU's replies a session delivered, in order */
struct Delivered {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> replies;
    size_t unflagged = 0; /* replies without ECUIO_FRAME_FLAG_REASSEMBLED */
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return replies.size();
    }
};

void on_reply(const ecuio_frame_t* f, void* user_data)
{
    if (f->arbitration_id != vcan_bus::kReply ||
        (f->flags & ECUIO_FRAME_FLAG_ERROR) != 0) {
        return;
    }
    auto* d = static_cast<Delivered*>(user_data);
    std::lock_guard<std::mutex> lock(d->mutex);
    d->replies.emplace_back(f->data, f->data + f->data_length);
    d->unflagged += (f->flags & ECUIO_FRAME_FLAG_REASSEMBLED) == 0;
}

/* What a session against the recording's ECU came to */
struct FaultRun {
    std::vector<std::vector<uint8_t>> whole;     /* the ECU's, in order */
    std::vector<std::vector<uint8_t>> delivered; /* the session's */
    size_t unflagged = 0; /* of them, not flagged reassembled */
    std::set<uint8_t> refused;
    std::set<uint8_t> answered;
    unsigned silent = 0, late = 0, bad_sequence = 0, stalled = 0;
    unsigned segmented = 0; /* segmented replies sent whole */
    unsigned bad_flow_controls = 0, unflowed = 0;
    ecuio_channel_support_t support{};
    int32_t error = -1;
};

/* Requests through the recording's ECU, the test car's engine start
 * playing at its own pace */
class SocketCanFaultTest : public SocketCanRequestTest
{
protected:
    /* Segmented replies sent whole before a run ends: each one is a
     * pack answered */
    static constexpr unsigned kSegmented = 100;

    void SetUp() override
    {
        SocketCanRequestTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        const std::string path = std::string(ECUIO_REPLAY_FIXTURES) +
                                 "/2026-10-25-engine-start/frames.csv";
        recording_ = vcan_bus::read_frames_csv(path.c_str());
        ASSERT_GT(recording_.size(), 1000u) << path;
        for (const vcan_bus::Message& m : recording_) {
            if (m.data.size() >= 2 && m.data[0] == 0x41 &&
                m.data[1] % 0x20 != 0) {
                recorded_.insert(m.data[1]);
            }
        }
    }

    /* A session until the probe's walk ends and kSegmented segmented
     * replies went out whole; then the ECU stops and the session takes
     * what's on the bus */
    void run(vcan_bus::RecordingEcu::Faults faults, FaultRun* out)
    {
        using Fate = vcan_bus::RecordingEcu::Fate;
        vcan_bus::RecordingEcu ecu(recording_, faults);
        ASSERT_TRUE(ecu.ok());
        Delivered d;
        Session s("?profile=obd2&identify=0&diagnose=0", on_reply, &d);
        ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
        const auto whole_segmented = [&] {
            return ecu.count(Fate::intact, true) + ecu.count(Fate::late, true);
        };
        EXPECT_TRUE(wait_for(
            [&] {
                ecuio_channel_support_t sup{};
                return ecuio_session_get_channel_support(s.get(), 0, &sup) ==
                           ECUIO_OK &&
                       (sup.flags & ECUIO_SUPPORT_COMPLETE) != 0;
            },
            5000ms));
        EXPECT_TRUE(
            wait_for([&] { return whole_segmented() >= kSegmented; }, 20000ms))
            << whole_segmented() << " segmented replies whole";
        ecu.stop();
        out->whole = ecu.whole();
        EXPECT_TRUE(
            wait_for([&] { return d.size() >= out->whole.size(); }, 1000ms));
        std::this_thread::sleep_for(50ms); /* nothing more comes */
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            out->delivered = d.replies;
            out->unflagged = d.unflagged;
        }
        out->refused = ecu.refused();
        out->answered = ecu.answered();
        out->silent = ecu.count(Fate::silent);
        out->late = ecu.count(Fate::late);
        out->bad_sequence = ecu.count(Fate::bad_sequence);
        out->stalled = ecu.count(Fate::stalled);
        out->segmented = whole_segmented();
        out->bad_flow_controls = ecu.bad_flow_controls();
        out->unflowed = ecu.unflowed();
        ASSERT_EQ(ecuio_session_get_channel_support(s.get(), 0, &out->support),
                  ECUIO_OK);
        uint32_t episodes = 0;
        ASSERT_EQ(
            ecuio_session_get_error_state(s.get(), &out->error, &episodes),
            ECUIO_OK);
    }

    /* What holds under every fault: the session delivered each reply the
     * bus carried whole, in order, flagged reassembled, and nothing else;
     * the walk read the
     * car's bitmaps; the rotation kept each PID the recording holds and
     * no other (the bitmaps' and the refusals' left), each was answered,
     * and packing held;
     * core's Flow Control was ISO 15765-4's; the link is healthy */
    void expect_in_step(const FaultRun& r)
    {
        EXPECT_EQ(r.delivered.size(), r.whole.size());
        EXPECT_TRUE(r.delivered == r.whole)
            << "a reply delivered that the bus didn't carry whole, or one "
               "it did left out";
        EXPECT_EQ(r.unflagged, 0u) << "each is ISO-TP's reassembly";
        EXPECT_NE(r.support.flags & ECUIO_SUPPORT_COMPLETE, 0u);
        EXPECT_EQ(r.support.ranges, 0x3Fu) << "the car's bitmaps, 00 to A0";
        EXPECT_EQ(r.support.requests - r.support.dropped, recorded_.size())
            << "the rotation keeps the PIDs the recording holds, and only them";
        EXPECT_EQ(r.answered, recorded_);
        EXPECT_NE(r.support.flags & ECUIO_SUPPORT_PACKED, 0u);
        EXPECT_EQ(r.support.flags & ECUIO_SUPPORT_UNPACKED, 0u)
            << "three packs in a row failed";
        EXPECT_GE(r.segmented, kSegmented);
        EXPECT_EQ(r.bad_flow_controls, 0u);
        EXPECT_EQ(r.unflowed, 0u);
        EXPECT_EQ(r.error, ECUIO_OK);
    }

    std::vector<vcan_bus::Message> recording_;
    std::set<uint8_t> recorded_; /* its PIDs, the bitmaps aside */
};

} // namespace

/* The test car's own replies: the probe walks its bitmaps, its PIDs
 * pack into segmented replies through core's ISO-TP, and every reply
 * reaches the session as the car sent it */
TEST_F(SocketCanFaultTest, TheTestCarsRepliesAnswerARequestingSession)
{
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run({}, &r));
    expect_in_step(r);
    EXPECT_EQ(r.silent + r.late + r.bad_sequence + r.stalled, 0u);
}

/* A Consecutive Frame out of sequence, and one later than N_Cr: the
 * message is never delivered, its pack fails and the next is asked;
 * the frames after it start nothing */
TEST_F(SocketCanFaultTest, ABrokenSegmentedReplyIsNeverDelivered)
{
    vcan_bus::RecordingEcu::Faults f;
    f.bad_sequence_every = 4;
    f.stalled_every = 7;
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run(f, &r));
    expect_in_step(r);
    EXPECT_GE(r.bad_sequence, kSegmented / 4);
    EXPECT_GE(r.stalled, kSegmented / 8);
}

/* No reply, and a reply after the engine's wait: the request times out
 * and the next goes out; the late reply is delivered but answers
 * nothing, so a late refusal drops no PID */
TEST_F(SocketCanFaultTest, SilenceAndLateRepliesLeaveTheSessionInStep)
{
    vcan_bus::RecordingEcu::Faults f;
    f.silent_every = 5;
    f.late_every = 7;
    FaultRun r;
    ASSERT_NO_FATAL_FAILURE(run(f, &r));
    expect_in_step(r);
    EXPECT_GE(r.silent, kSegmented / 5);
    EXPECT_GE(r.late, kSegmented / 8);
}

/* An ECU gone quiet is VEHICLE_TIMEOUT, on which the worker judges a
 * link term at once, on the kernel's clock its frames count on, and a set
 * attached meanwhile at its next pass; the ECU's return ends it (ADR-0063
 * step 6) */
TEST_F(SocketCanRequestTest, AQuietEcuFiresALinkTermAndItsReturnEndsIt)
{
    struct Events {
        std::mutex m;
        std::vector<ecuio_condition_event_t> all;
        static void cb(const ecuio_condition_event_t* e, uint32_t n, void* u)
        {
            auto* self = static_cast<Events*>(u);
            std::lock_guard<std::mutex> lock(self->m);
            self->all.insert(self->all.end(), e, e + n);
        }
        /* An event into state with code, flagged LINK; of that attach's
         * set when generation isn't 0 */
        bool has(uint32_t state, int32_t code, uint32_t generation = 0)
        {
            std::lock_guard<std::mutex> lock(m);
            for (const ecuio_condition_event_t& e : all) {
                if (e.state == state && e.code == code &&
                    (generation == 0 || e.generation == generation) &&
                    (e.flags & ECUIO_CONDITION_LINK) != 0) {
                    return true;
                }
            }
            return false;
        }
    } ev;
    auto ecu = std::make_unique<vcan_bus::Ecu>();
    Delivered d;
    Session s("?profile=obd2&identify=0&diagnose=0", on_reply, &d);
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    const auto attach_quiet = [&] {
        const std::string json =
            R"({"havne_conditions":1,"conditions":[{"key":"quiet",)"
            R"("name":"Quiet","when":{"link":"timeout"}}]})";
        ecuio_condition_set_t* set = nullptr;
        ASSERT_EQ(ecuio_condition_set_load(json.data(), json.size(), &set),
                  ECUIO_OK);
        ASSERT_EQ(ecuio_session_set_conditions(s.get(), set, nullptr), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_condition_set_free(set);
    };
    ASSERT_NO_FATAL_FAILURE(attach_quiet());
    ASSERT_EQ(ecuio_register_conditions_callback(s.get(), Events::cb, &ev),
              ECUIO_OK);
    ASSERT_TRUE(wait_for([&] { return d.size() >= 20; }, 5000ms));
    ecu.reset();
    ASSERT_TRUE(wait_for(
        [&] { return ev.has(ECUIO_CONDITION_ON, ECUIO_ERR_VEHICLE_TIMEOUT); },
        5000ms))
        << "no link event for the quiet ECU";
    /* A set attached during the quiet, with no frame to take it */
    ASSERT_NO_FATAL_FAILURE(attach_quiet());
    ASSERT_TRUE(wait_for(
        [&] {
            return ev.has(ECUIO_CONDITION_ON, ECUIO_ERR_VEHICLE_TIMEOUT, 2);
        },
        2000ms))
        << "the set attached during the quiet waited for a frame";
    ecu = std::make_unique<vcan_bus::Ecu>();
    ASSERT_TRUE(
        wait_for([&] { return ev.has(ECUIO_CONDITION_OFF, ECUIO_OK); }, 5000ms))
        << "the ECU's return ends it";
    std::lock_guard<std::mutex> lock(ev.m);
    for (size_t i = 1; i < ev.all.size(); ++i) {
        EXPECT_GE(ev.all[i].timestamp_us, ev.all[i - 1].timestamp_us) << i;
    }
}

namespace {

/* The engine ECU on a raw socket, for the close's cases (ADR-0060): 10 03
 * entered, 10 01 left, 3E 00 and D200 answered at once, until hold()
 * keeps the next D200's answer: sent late after the request, as a First
 * Frame late and its Consecutive Frame on the tester's Flow Control
 * (late_segmented), never sent (silent), or never sent with a 7F 22 78
 * every 40 ms in its place (pending). Each time is the ECU thread's
 * steady_clock. */
class HoldingEcu
{
public:
    enum class Hold { none, late, late_segmented, silent, pending };
    using Clock = std::chrono::steady_clock;
    struct Seen {
        unsigned reads = 0;           /* D200 answered */
        Clock::time_point held_at{};  /* the held D200 arrived */
        Clock::time_point reply_at{}; /* its late answer's last frame */
        Clock::time_point left_at{};  /* the first 10 01 after the hold */
    };

    HoldingEcu() : raw_(vcan_bus::open_raw()), stop_(::eventfd(0, EFD_CLOEXEC))
    {
        if (ok()) {
            thread_ = std::thread([this] { run(); });
        }
    }
    HoldingEcu(const HoldingEcu&) = delete;
    HoldingEcu& operator=(const HoldingEcu&) = delete;
    ~HoldingEcu()
    {
        if (thread_.joinable()) {
            const uint64_t one = 1;
            (void)!::write(stop_.get(), &one, sizeof(one));
            thread_.join();
        }
    }
    bool ok() const { return raw_.ok() && stop_.ok(); }
    void hold(Hold how, std::chrono::milliseconds late)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_ = how;
        late_ = late;
    }
    /* Waits up to limit for the held D200 */
    bool held(std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock, limit, [&] { return seen_.held_at != Clock::time_point{}; });
    }
    Seen seen()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return seen_;
    }

private:
    void send(std::initializer_list<uint8_t> bytes)
    {
        can_frame f{};
        f.can_id = vcan_bus::kReply;
        f.can_dlc = 8;
        std::memset(f.data, vcan_bus::kPad, sizeof f.data);
        f.data[0] = static_cast<uint8_t>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), f.data + 1);
        (void)!::write(raw_.get(), &f, sizeof f);
    }
    /* One frame as given, PCI included */
    void send_frame(std::initializer_list<uint8_t> bytes)
    {
        can_frame f{};
        f.can_id = vcan_bus::kReply;
        f.can_dlc = 8;
        std::memset(f.data, vcan_bus::kPad, sizeof f.data);
        std::copy(bytes.begin(), bytes.end(), f.data);
        (void)!::write(raw_.get(), &f, sizeof f);
    }

    /* The tester's Flow Control: the held answer's Consecutive Frame */
    void flow_control(Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hold_ == Hold::late_segmented && first_sent_ &&
            seen_.reply_at == Clock::time_point{}) {
            send_frame({0x21, 0xBB, 0xCC, 0xDD, 0xEE});
            seen_.reply_at = now;
        }
    }

    void take(const uint8_t* q, size_t n, Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (n == 2 && q[0] == 0x10 && q[1] == 0x03) {
            send({0x50, 0x03, 0x00, 0x32, 0x01, 0xF4});
        } else if (n == 2 && q[0] == 0x10 && q[1] == 0x01) {
            if (seen_.held_at != Clock::time_point{} &&
                seen_.left_at == Clock::time_point{}) {
                seen_.left_at = now;
            }
            send({0x50, 0x01, 0x00, 0x32, 0x01, 0xF4});
        } else if (n == 2 && q[0] == 0x3E && q[1] == 0x00) {
            send({0x7E, 0x00});
        } else if (n == 3 && q[0] == 0x22 && q[1] == 0xD2 && q[2] == 0x00) {
            if (hold_ == Hold::none || seen_.held_at != Clock::time_point{}) {
                send({0x62, 0xD2, 0x00, 0x12, 0x34});
                ++seen_.reads;
                return;
            }
            seen_.held_at = now;
            next_ = (hold_ == Hold::late || hold_ == Hold::late_segmented)
                        ? now + late_
                        : now;
            cv_.notify_all();
        } else if (n >= 1) {
            send({0x7F, q[0], 0x11});
        }
    }

    /* Sends the held answer's next step once it's due; the wait until
     * the step after, -1 for none */
    int timed(Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool owed =
            seen_.held_at != Clock::time_point{} &&
            seen_.left_at == Clock::time_point{} &&
            ((hold_ == Hold::late && seen_.reply_at == Clock::time_point{}) ||
             (hold_ == Hold::late_segmented && !first_sent_) ||
             hold_ == Hold::pending);
        if (!owed) {
            return -1;
        }
        if (now < next_) {
            return static_cast<int>(
                std::chrono::ceil<std::chrono::milliseconds>(next_ - now)
                    .count());
        }
        if (hold_ == Hold::late) {
            send({0x62, 0xD2, 0x00, 0x12, 0x34});
            seen_.reply_at = Clock::now();
            return -1;
        }
        if (hold_ == Hold::late_segmented) {
            send_frame({0x10, 0x0A, 0x62, 0xD2, 0x00, 0x12, 0x34, 0xAA});
            first_sent_ = true;
            return -1;
        }
        send({0x7F, 0x22, 0x78});
        next_ = now + 40ms;
        return 40;
    }

    void run()
    {
        for (;;) {
            const int timeout = timed(Clock::now());
            pollfd p[2] = {{raw_.get(), POLLIN, 0}, {stop_.get(), POLLIN, 0}};
            if (::poll(p, 2, timeout) < 0 && errno != EINTR) {
                return;
            }
            if (p[1].revents != 0) {
                return;
            }
            if ((p[0].revents & POLLIN) == 0) {
                continue;
            }
            can_frame f{};
            if (::read(raw_.get(), &f, sizeof f) !=
                    static_cast<ssize_t>(sizeof f) ||
                f.can_id != vcan_bus::kRequest || f.can_dlc < 1) {
                continue;
            }
            if ((f.data[0] >> 4) == 3) {
                flow_control(Clock::now());
            } else if ((f.data[0] >> 4) == 0 && f.data[0] + 1u <= f.can_dlc) {
                take(f.data + 1, f.data[0], Clock::now());
            }
        }
    }

    vcan_bus::Fd raw_;
    vcan_bus::Fd stop_;
    std::mutex mutex_;
    std::condition_variable cv_;
    Hold hold_ = Hold::none;
    std::chrono::milliseconds late_{0};
    bool first_sent_ = false;
    Clock::time_point next_{};
    Seen seen_;
    std::thread thread_;
};

/* What a close over a held D200 came to: the ECU's times, when the close
 * was asked and returned, and D200's reply timing after the close */
struct CloseRun {
    HoldingEcu::Seen seen;
    HoldingEcu::Clock::time_point asked{};
    HoldingEcu::Clock::time_point closed{};
    ecuio_request_timing_t d200{};
};

/* A session reading D200 in the extended session; its next D200 held as
 * how says, and the close asked once that read is out */
void close_over_a_held_read(HoldingEcu::Hold how,
                            std::chrono::milliseconds late, CloseRun* out)
{
    HoldingEcu ecu;
    ASSERT_TRUE(ecu.ok());
    Session s("?profile=uds-generic&did=D200:u16&session=03"
              "&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    ASSERT_TRUE(wait_for([&] { return ecu.seen().reads >= 3; }, 5000ms))
        << "D200 read " << ecu.seen().reads << " times";
    ecu.hold(how, late);
    ASSERT_TRUE(ecu.held(2000ms));
    out->asked = HoldingEcu::Clock::now();
    ecuio_session_close(s.get());
    out->closed = HoldingEcu::Clock::now();
    EXPECT_TRUE(wait_for(
        [&] { return ecu.seen().left_at != HoldingEcu::Clock::time_point{}; },
        1000ms))
        << "no 10 01 after the held read";
    out->seen = ecu.seen();
    ecuio_session_timing_t t{};
    std::vector<ecuio_request_timing_t> rows(ECUIO_PROFILE_MAX_REQUESTS);
    uint32_t count = 0;
    ASSERT_EQ(ecuio_session_get_timing(s.get(), &t, rows.data(),
                                       static_cast<uint32_t>(rows.size()),
                                       &count),
              ECUIO_OK);
    for (uint32_t i = 0; i < count && i < rows.size(); ++i) {
        if (rows[i].length == 3 && rows[i].request[0] == 0x22 &&
            rows[i].request[1] == 0xD2 && rows[i].request[2] == 0x00) {
            out->d200 = rows[i];
        }
    }
}

} // namespace

/* ADR-0060's close over a request still out: the module answering it
 * hears 10 01 after its answer, never before, so it isn't left in the
 * extended session to wait out S3; a segmented answer gets its Flow
 * Control at the close and ends before 10 01 too */
TEST_F(SocketCanRequestTest, TheCloseWaitsForTheReplyStillOutBefore1001)
{
    for (const HoldingEcu::Hold how :
         {HoldingEcu::Hold::late, HoldingEcu::Hold::late_segmented}) {
        SCOPED_TRACE(how == HoldingEcu::Hold::late ? "single" : "segmented");
        /* a close asked after the answer went out (a loaded test thread
         * missed its 40 ms) proves nothing: run again, three runs at most */
        CloseRun r;
        for (int run = 0; run < 3; ++run) {
            r = CloseRun{};
            ASSERT_NO_FATAL_FAILURE(close_over_a_held_read(how, 40ms, &r));
            if (r.seen.reply_at == HoldingEcu::Clock::time_point{} ||
                r.asked < r.seen.reply_at) {
                break;
            }
        }
        ASSERT_NE(r.seen.reply_at, HoldingEcu::Clock::time_point{})
            << "the answer never ended (no Flow Control for a segmented one)";
        ASSERT_LT(r.asked, r.seen.reply_at)
            << "the close was asked after the answer in three runs: nothing "
               "was still out";
        EXPECT_GT(r.seen.left_at, r.seen.reply_at)
            << "10 01 went out before the answer to the request still out";
        EXPECT_LT(r.seen.left_at - r.seen.reply_at, 100ms)
            << "the close waited on past the answer";
        /* the final timing, published after the close: each D200 the ECU
         * answered at once, and the held one, answered or timed out */
        EXPECT_EQ(r.d200.answered + r.d200.timeouts, r.seen.reads + 1)
            << "the held answer never reached the engine at the close";
        EXPECT_LT(r.closed - r.asked, 1000ms);
    }
}

/* An answer that never comes, the module silent or answering 78 on and
 * on, holds the close for the request's wait and no longer: 10 01 goes
 * out within the engine's wait (100 ms) of the close */
TEST_F(SocketCanRequestTest, AnAnswerThatNeverComesHoldsTheCloseToItsBound)
{
    for (const HoldingEcu::Hold how :
         {HoldingEcu::Hold::silent, HoldingEcu::Hold::pending}) {
        SCOPED_TRACE(how == HoldingEcu::Hold::silent ? "silent" : "pending");
        CloseRun r;
        ASSERT_NO_FATAL_FAILURE(close_over_a_held_read(how, 0ms, &r));
        EXPECT_EQ(r.seen.reply_at, HoldingEcu::Clock::time_point{});
        EXPECT_GE(r.seen.left_at - r.seen.held_at, 90ms)
            << "10 01 went out before the request's wait ended";
        EXPECT_LT(r.seen.left_at - r.asked, 400ms)
            << "the close waited past its bound";
        EXPECT_LT(r.closed - r.asked, 1000ms);
    }
}

TEST_F(SocketCanRequestTest, AModulesCodesAreReadOnVcan)
{
    vcan_bus::Ecu ecu;
    Session s("?profile=obd2&probe=0&identify=0&diagnose=0");
    ASSERT_EQ(s.status(), ECUIO_OK) << s.detail();
    uint32_t id = 0;
    ASSERT_EQ(ecuio_session_read_module_codes(s.get(), 0, 0, &id), ECUIO_OK);
    ecuio_module_codes_t r{};
    ASSERT_TRUE(wait_for(
        [&] {
            return ecuio_session_get_module_codes(s.get(), 0, &r) == ECUIO_OK &&
                   r.read_id >= id &&
                   (r.flags & ECUIO_MODULE_CODES_COMPLETE) != 0;
        },
        8000ms));
    EXPECT_EQ(r.rx_id, 0x7E8u);
    fake_diagnostics::expect_module_codes(r, 0xAF);
}
