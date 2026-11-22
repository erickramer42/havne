// File/layer: tests/test_elm327_session_stream.cpp | GTest, HARNESS tier,
//             headless CI job (every platform)
// Testing: HARNESS: Elm327StreamTest, Elm327SessionTest (test_elm327)

#include "test_elm327_support.h"

/* ---- App-fed streams (ADR-0028, ecuio/stream.h) -------------------------
 * The fake ELM327 behind a registered stream, a pump thread playing the
 * app as QtBluetooth would: open answered later from its own thread,
 * write fed to the adapter, the adapter's output pushed as received. The
 * real registry, opener and port, on every platform. */

namespace {

/* The open's wait for a scope, then the 10 s again */
class OpenWait
{
public:
    explicit OpenWait(uint32_t ms) { ecuio_elm327_set_stream_open_ms(ms); }
    ~OpenWait() { ecuio_elm327_set_stream_open_ms(0); }
    OpenWait(const OpenWait&) = delete;
    OpenWait& operator=(const OpenWait&) = delete;
};

long long ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
}

} // namespace

/* The app's failed is a hard failure: close, then open again on the
 * backoff, the setup run, values again, one episode, no reconnect */
TEST(Elm327StreamTest, AFailedLinkIsAHardFailureThatOpensAgain)
{
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    Session s;
    ASSERT_EQ(s.create(bt_uri("?poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 2; }, 5000));

    app->fail("the adapter went out of range");
    uint32_t episodes = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            return s.state(&episodes) == ECUIO_ERR_TRANSPORT_HARD_FAILURE ||
                   episodes > 0;
        },
        5000))
        << "the failure is a hard failure";
    const size_t before = s.cap.count("41 0C 1F 40");
    EXPECT_TRUE(wait_for(
        [&] {
            return app->opens() == 2u && s.state() == ECUIO_OK &&
                   s.cap.count("41 0C 1F 40") > before;
        },
        5000))
        << "opened again, values again";
    EXPECT_EQ(app->closes(), 1u) << "closed before the reopen";
    s.state(&episodes);
    EXPECT_EQ(episodes, 1u);
}

/* An open the app never answers is refused after the wait, said as one,
 * and the app is told to stop connecting */
TEST(Elm327StreamTest, AnOpenThatNeverAnswersIsRefusedAfterItsWait)
{
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    app->answer_with(AppStream::answer::nothing);
    OpenWait wait(300);
    ecuio_session_t* s = nullptr;
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(ecuio_session_create(bt_uri("?poll=0x0C").c_str(), &s),
              ECUIO_ERR_LOAD_FAILED);
    const long long took = ms_since(from);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string(kBt) + ": the link didn't open within 1 s");
    EXPECT_GE(took, 280);
    EXPECT_LT(took, 2000);
    EXPECT_EQ(s, nullptr);
    EXPECT_EQ(app->closes(), 1u);
    EXPECT_EQ(app->unregister(), ECUIO_OK) << "free again after the refusal";
}

/* The app's reason for a failed open is the connect's detail, on one
 * line */
TEST(Elm327StreamTest, AFailedOpenSaysWhy)
{
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    app->answer_with(AppStream::answer::failed, "pairing was\nrefused");
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create(bt_uri("?poll=0x0C").c_str(), &s),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string(kBt) + ": pairing was refused");
    EXPECT_EQ(s, nullptr);
    EXPECT_EQ(app->closes(), 1u);
}

/* Closing the session ends a reopen's wait at once, not after 10 s */
TEST(Elm327StreamTest, ClosingEndsAReopensWait)
{
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    auto s = std::make_unique<Session>();
    ASSERT_EQ(s->create(bt_uri("?poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    s->start();
    ASSERT_TRUE(
        wait_for([&] { return s->cap.count("41 0C 1F 40") >= 1; }, 5000));
    app->answer_with(AppStream::answer::nothing);
    app->fail("the link dropped");
    ASSERT_TRUE(wait_for([&] { return app->opens() == 2u; }, 5000))
        << "the worker waits on its reopen";
    const auto from = std::chrono::steady_clock::now();
    s.reset();
    EXPECT_LT(ms_since(from), 1000);
    EXPECT_EQ(app->closes(), 2u) << "the failure's, then the waiting open's";
    EXPECT_EQ(app->unregister(), ECUIO_OK);
}

/* A name nothing registered is no such port, as a missing COM port is;
 * a stream has no serial rate */
TEST(Elm327StreamTest, AnUnregisteredNameIsNoSuchPort)
{
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create("elm327://ble:AA:BB?poll=0x0C", &s),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "ble:AA:BB: no such port");
    EXPECT_EQ(s, nullptr);

    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    EXPECT_EQ(ecuio_session_create(bt_uri("?serial=115200").c_str(), &s),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("?serial= is a serial port's rate, and ") + kBt +
                  " is an app's stream, which has none");
    EXPECT_EQ(app->opens(), 0u) << "refused before the open";
}

/* The registry's refusals: missing arguments, names of another shape,
 * nonzero growth space, a name taken */
TEST(Elm327StreamTest, TheRegistryRefusesWhatItCantTake)
{
    ecuio_stream_callbacks_t cb{};
    cb.open = [](void*) {};
    cb.write = [](void*, const uint8_t*, uint32_t) {};
    cb.close = [](void*) {};
    ecuio_stream_t* s = nullptr;
    EXPECT_EQ(ecuio_stream_register(nullptr, &cb, &s), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_stream_register("bt:x", nullptr, &s),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_stream_register("bt:x", &cb, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    ecuio_stream_callbacks_t no_write = cb;
    no_write.write = nullptr;
    EXPECT_EQ(ecuio_stream_register("bt:x", &no_write, &s),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_stream_unregister(nullptr), ECUIO_ERR_NULL_ARGUMENT);

    const std::string too_long = "bt:" + std::string(125, 'a');
    for (const std::string& bad :
         {std::string("BT:x"), std::string("bt:"), std::string(":x"),
          std::string("bt"), std::string("b1:x"), std::string("bt:a b"),
          std::string("bt:a?b"), std::string("bt:a\tb"), std::string("COM5"),
          std::string("192.168.0.10:35000"), too_long}) {
        EXPECT_EQ(ecuio_stream_register(bad.c_str(), &cb, &s),
                  ECUIO_ERR_INVALID_ARGUMENT)
            << bad;
    }
    ecuio_stream_callbacks_t flagged = cb;
    flagged.flags = 1;
    EXPECT_EQ(ecuio_stream_register("bt:x", &flagged, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    ecuio_stream_callbacks_t grown = cb;
    grown.reserved[4] = 1;
    EXPECT_EQ(ecuio_stream_register("bt:x", &grown, &s),
              ECUIO_ERR_INVALID_ARGUMENT);

    const std::string longest = "bt:" + std::string(124, 'a');
    ASSERT_EQ(ecuio_stream_register(longest.c_str(), &cb, &s), ECUIO_OK);
    ecuio_stream_t* again = nullptr;
    EXPECT_EQ(ecuio_stream_register(longest.c_str(), &cb, &again),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a name taken";
    EXPECT_EQ(ecuio_stream_unregister(s), ECUIO_OK);
    ASSERT_EQ(ecuio_stream_register(longest.c_str(), &cb, &again), ECUIO_OK)
        << "free once unregistered";
    EXPECT_EQ(ecuio_stream_unregister(again), ECUIO_OK);
}

/* Bytes past the buffer can't be resynchronized: the next read is a
 * failed link, the session hard-fails, opens again and recovers */
TEST(Elm327StreamTest, BytesPastTheBufferFailTheLink)
{
    auto app = std::make_unique<AppStream>(kBt);
    ASSERT_EQ(app->registered(), ECUIO_OK);
    Session s;
    ASSERT_EQ(s.create(bt_uri("?poll=0x0C")), ECUIO_OK)
        << ecuio_last_error_detail();
    s.start();
    ASSERT_TRUE(
        wait_for([&] { return s.cap.count("41 0C 1F 40") >= 1; }, 5000));

    app->push(std::string(ELM327_STREAM_BUFFER + 1, 'X'));
    uint32_t episodes = 0;
    EXPECT_TRUE(wait_for(
        [&] {
            s.state(&episodes);
            return episodes > 0;
        },
        5000))
        << "the overflow fails the link";
    const size_t before = s.cap.count("41 0C 1F 40");
    EXPECT_TRUE(wait_for(
        [&] {
            return app->opens() == 2u && s.state() == ECUIO_OK &&
                   s.cap.count("41 0C 1F 40") > before;
        },
        5000))
        << "opened again, values again";
}

/* The port itself: reads time out and carry what the app pushed; pushes
 * outside an open are discarded; one session at a time; a failure ends
 * reads and writes */
TEST(Elm327StreamTest, ThePortTakesOnlyTheOpenLinksBytes)
{
    auto app = std::make_unique<AppStream>("bt:port");
    ASSERT_EQ(app->registered(), ECUIO_OK);
    app->push("before any open");

    ecuio_elm327_port_t port{};
    char why[128] = "";
    ASSERT_EQ(elm327_stream_open("bt:port", nullptr, &port, why, sizeof(why)),
              ECUIO_OK)
        << why;
    ecuio_elm327_port_t second{};
    EXPECT_EQ(elm327_stream_open("bt:port", nullptr, &second, why, sizeof(why)),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(why, "the link is in use by another session");

    uint8_t buf[64];
    const auto from = std::chrono::steady_clock::now();
    EXPECT_EQ(port.ops->read(port.impl, buf, sizeof(buf), 80), 0)
        << "nothing from before the open";
    EXPECT_GE(ms_since(from), 70);

    ASSERT_TRUE(port.ops->write(port.impl,
                                reinterpret_cast<const uint8_t*>("ATI\r"), 4));
    std::string got;
    ASSERT_TRUE(wait_for(
        [&] {
            const int32_t n = port.ops->read(port.impl, buf, sizeof(buf), 20);
            if (n > 0) {
                got.append(reinterpret_cast<const char*>(buf),
                           static_cast<size_t>(n));
            }
            return got.find('>') != std::string::npos;
        },
        2000));
    EXPECT_NE(got.find("ELM327 v1.4b"), std::string::npos) << got;
    port.ops->close(port.impl);
    EXPECT_EQ(app->closes(), 1u);

    app->push("after the close");
    ASSERT_EQ(elm327_stream_open("bt:port", nullptr, &port, why, sizeof(why)),
              ECUIO_OK)
        << why;
    EXPECT_EQ(port.ops->read(port.impl, buf, sizeof(buf), 0), 0)
        << "nothing from after the close";
    app->fail("gone");
    EXPECT_EQ(port.ops->read(port.impl, buf, sizeof(buf), 0), -1);
    EXPECT_FALSE(
        port.ops->write(port.impl, reinterpret_cast<const uint8_t*>("\r"), 1));
    port.ops->close(port.impl);
}

/* A condition with a term on a signal the session reports it doesn't
 * get, and has had no sample of, says so once the session runs
 * (ECUIO_CONDITION_WHY_NOT_RECEIVED): a PID the bitmaps leave out
 * (unsupported), a DID the ECU refuses (7F 22 31, its request dropped)
 * and a signal computed from it, a field past its replies (absent); any
 * such term of an all, whatever its other terms read, every term of an
 * any (an any with a received term reads as that term judges it). A
 * received signal's conditions are judged, a computed one's too */
TEST_F(Elm327SessionTest, AConditionOnASignalTheSessionDoesntGetSaysSo)
{
    Withheld speed({0x0D});
    const std::string json =
        R"({"havne_signals": 1, "name": "Not received",)"
        R"( "source": {"name": "test_elm327_session"}, "extends": "obd2",)"
        R"( "ecus": [{"id": "pcm", "rx": "7E8"}], "signals": [)"
        R"({"key": "good", "name": "Good", "ecu": "pcm",)"
        R"( "read": {"service": "22", "number": "1E1C"},)"
        R"( "field": {"byte": 0, "bits": 16}, "unit": "kPa"},)"
        R"({"key": "bad", "name": "Bad", "ecu": "pcm",)"
        R"( "read": {"service": "22", "number": "ABCD"},)"
        R"( "field": {"byte": 0, "bits": 16}, "unit": "kPa"},)"
        R"({"key": "twice", "name": "Twice", "kind": "computed",)"
        R"( "expr": "bad * 2", "unit": "kPa"},)"
        R"({"key": "fine", "name": "Fine", "kind": "computed",)"
        R"( "expr": "good * 2", "unit": "kPa"},)"
        R"({"key": "past", "name": "Past", "ecu": "pcm",)"
        R"( "read": {"service": "22", "number": "1E1C"},)"
        R"( "field": {"byte": 4, "bits": 8}, "unit": "kPa"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(json.data(), json.size(), &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const std::string profile = ecuio_signal_file_profile(file);
    ASSERT_EQ(ecuio_profile_register(profile.c_str(),
                                     ecuio_signal_file_set(file), nullptr),
              ECUIO_OK);
    {
        WorkerHold hold;
        Session s;
        WorkerHold::Release unhold{hold};
        ASSERT_EQ(
            s.create(uri("?profile=" + profile + "&identify=0&diagnose=0")),
            ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(hold.attach(s.s), ECUIO_OK);
        const auto term = [](const char* ref, const char* unit) {
            return std::string(R"({"signal":[")") + ref + R"("],"unit":")" +
                   unit + R"(","above":1})";
        };
        const std::string rpm = term("obd2.rpm", "RPM");
        const std::string kmh = term("obd2.speed", "km/h");
        const std::string bad = term("key:bad", "kPa");
        /* False on every value: an any of it stays unjudged, never
         * judged, while its other term can't be judged */
        const std::string fine_low =
            R"({"signal":["key:fine"],"unit":"kPa","below":1})";
        /* False on every value: an all of it is judged OFF at once */
        const std::string rpm_low =
            R"({"signal":["obd2.rpm"],"unit":"RPM","below":1})";
        const std::string whens[] = {
            rpm,                                         /* 0 received */
            bad,                                         /* 1 refused */
            term("key:twice", "kPa"),                    /* 2 computed */
            kmh,                                         /* 3 unsupported */
            R"({"all":[)" + rpm + "," + kmh + "]}",      /* 4 */
            R"({"any":[)" + rpm + "," + kmh + "]}",      /* 5 */
            R"({"any":[)" + kmh + "," + bad + "]}",      /* 6 */
            term("key:good", "kPa"),                     /* 7 received */
            term("key:fine", "kPa"),                     /* 8 computed */
            term("key:past", "kPa"),                     /* 9 absent */
            R"({"any":[)" + kmh + "," + fine_low + "]}", /* 10 */
            R"({"all":[)" + rpm_low + "," + bad + "]}",  /* 11 */
        };
        std::string conditions = R"({"havne_conditions":1,"conditions":[)";
        for (size_t i = 0; i < 12; ++i) {
            const std::string k = std::to_string(i);
            conditions += (i == 0 ? "" : ",") + std::string(R"({"key":"c)") +
                          k + R"(","name":"C)" + k + R"(","when":)" + whens[i] +
                          "}";
        }
        conditions += "]}";
        ecuio_condition_set_t* set = nullptr;
        ASSERT_EQ(ecuio_condition_set_load(conditions.data(), conditions.size(),
                                           &set),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_EQ(ecuio_session_set_conditions(s.s, set, file), ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_condition_set_free(set);
        ecuio_condition_state_t st[12] = {};
        uint32_t n = 0;
        ASSERT_EQ(ecuio_session_get_condition_states(s.s, st, 12, &n),
                  ECUIO_OK);
        for (size_t i = 0; i < 12; ++i) {
            EXPECT_EQ(st[i].flags, ECUIO_CONDITION_STALE)
                << i << ": nothing judged before the session runs";
        }
        s.start();
        const auto not_received = [&](size_t i) {
            return st[i].flags == ECUIO_CONDITION_UNAVAILABLE &&
                   st[i].why == ECUIO_CONDITION_WHY_NOT_RECEIVED;
        };
        const auto settled = [&] {
            EXPECT_EQ(ecuio_session_get_condition_states(s.s, st, 12, &n),
                      ECUIO_OK);
            return st[0].since_us != 0 && st[7].since_us != 0 &&
                   st[8].since_us != 0 && not_received(1) && not_received(2) &&
                   not_received(3) && not_received(4) && not_received(6) &&
                   not_received(9) && not_received(11);
        };
        /* Absent takes three of 1E1C's replies: a loaded machine's
         * rotation has needed more than 5 s */
        EXPECT_TRUE(wait_for(settled, 20000));
        for (size_t i = 0; i < 12; ++i) {
            const bool want = i != 0 && i != 5 && i != 7 && i != 8 && i != 10;
            EXPECT_EQ(not_received(i), want)
                << i << ": flags " << st[i].flags << ", why " << st[i].why;
        }
        EXPECT_NE(st[5].since_us, 0u) << "an any judged by its engine speed";
        EXPECT_EQ(st[10].flags, ECUIO_CONDITION_STALE)
            << "an any with a term on a signal received, false, is STALE "
               "and not marked";
        EXPECT_EQ(st[10].since_us, 0u);
        EXPECT_NE(st[11].since_us, 0u)
            << "an all judged OFF by its engine speed, and still marked";

        /* One reply of the refused DID, dispatched here with the worker
         * parked, judges its conditions; once that sample passes its age
         * bound the condition is STALE in the state it was judged to, not
         * unavailable: sampled once, never marked */
        ecuio_frame_t f{};
        f.arbitration_id = 0x7E8;
        f.flags = ECUIO_FRAME_FLAG_REASSEMBLED;
        f.timestamp_us = st[7].since_us;
        const uint8_t reply[] = {0x62, 0xAB, 0xCD, 0x00, 0x05};
        std::memcpy(f.data, reply, sizeof reply);
        f.data_length = sizeof reply;
        ASSERT_TRUE(hold.hold(5000)) << "the worker parks at its next frame";
        ecuio_session_dispatch_callbacks(s.s, &f);
        hold.release();
        ASSERT_EQ(ecuio_session_get_condition_states(s.s, st, 12, &n),
                  ECUIO_OK);
        EXPECT_EQ(st[11].flags & ECUIO_CONDITION_UNAVAILABLE, 0u)
            << "its refused DID sampled: the all is judged, OFF";
        EXPECT_EQ(st[11].state, ECUIO_CONDITION_OFF);
        const auto kept = [&] {
            EXPECT_EQ(ecuio_session_get_condition_states(s.s, st, 12, &n),
                      ECUIO_OK);
            return st[1].since_us != 0 && st[1].flags == ECUIO_CONDITION_STALE;
        };
        EXPECT_TRUE(wait_for(kept, 8000))
            << "flags " << st[1].flags << ", why " << st[1].why;
        EXPECT_EQ(st[1].state, ECUIO_CONDITION_ON);
        EXPECT_EQ(st[1].why, ECUIO_CONDITION_WHY_NONE);
    }
    ASSERT_EQ(ecuio_profile_unregister(profile.c_str()), ECUIO_OK);
    ecuio_signal_file_free(file);
}
