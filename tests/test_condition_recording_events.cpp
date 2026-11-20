// File/layer: tests/test_condition_recording_events.cpp | test_core, Qt-free, C++17
// Testing: UNIT: ConditionRecordingTest (test_core)

#include "test_condition_recording_support.h"

namespace {

/* Throttle position (PID 11, a 255th of the travel per count) likewise */
void throttle(ecuio_session_t* s, uint64_t t_us, uint8_t a)
{
    static ecuio_frame_t f;
    std::memset(&f, 0, sizeof f);
    f.arbitration_id = ECUIO_OBD2_ECU_ARB_ID;
    f.timestamp_us = t_us;
    f.data[0] = 0x41;
    f.data[1] = 0x11;
    f.data[2] = a;
    f.data_length = 3;
    ecuio_session_dispatch_callbacks(s, &f);
}

/* The drop lines that count lost condition events, in the file's order */
std::vector<std::string> lost_lines(const fs::path& folder)
{
    std::vector<std::string> out;
    for (const std::string& l : kind_lines(folder, "drop")) {
        if (l.rfind("the recording lost ", 0) == 0) {
            out.push_back(l);
        }
    }
    return out;
}

std::string lost_line(uint64_t n)
{
    return "the recording lost " + std::to_string(n) +
           (n == 1 ? " condition event" : " condition events") +
           ": the disk fell behind";
}

/* The lost condition events the reader counts in a recording */
uint64_t events_lost_read(const fs::path& folder)
{
    ecuio_recording_t* rec = nullptr;
    EXPECT_EQ(ecuio_recording_open(folder.generic_u8string().c_str(), nullptr,
                                   nullptr, &rec),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_conditions_t c{};
    EXPECT_EQ(ecuio_recording_get_conditions(rec, &c), ECUIO_OK);
    ecuio_recording_close(rec);
    return c.events_lost;
}

} // namespace

TEST(ConditionRecordingTest, AFullRingCarriesEveryEvent)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; ++i) {
        engine_speed(r.session.s, 1000000 + i * 10000u,
                     i % 2 == 0 ? 3000 : 1000);
    }
    ASSERT_GT(r.state().records_dropped, 0u) << "the ring filled";
    ecuio_recorder_test_hold(r.recorder, 0);
    /* The writer drains the ring; then a frame takes the carry in ahead */
    r.drained();
    engine_speed(r.session.s, 5000000, 3000); /* a change: its event too */
    const uint64_t dropped = r.state().records_dropped;
    EXPECT_EQ(r.state().conditions_lost, 0u);
    r.close();

    const std::vector<Cond> lines = cond_lines(r.folder);
    ASSERT_EQ(lines.size(), r.events.all.size())
        << "every event written though " << dropped << " frames were not";
    for (size_t i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].t_us, r.events.all[i].timestamp_us) << i;
    }
    EXPECT_TRUE(kind_lines(r.folder, "drop").size() >= 1u)
        << "the frames dropped are said";
}

TEST(ConditionRecordingTest, ChangesPastTheCarryAreCountedLost)
{
    /* The carry and its count go in through the ring, or the close writes
     * them with the writer still held */
    for (const bool through_the_ring : {true, false}) {
        SCOPED_TRACE(through_the_ring ? "through the ring" : "at the close");
        Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
        ASSERT_NE(r.recorder, nullptr);
        r.record();
        ecuio_recorder_test_hold(r.recorder, 1);
        constexpr int kFrames = 600;
        for (int i = 0; i < kFrames; ++i) {
            engine_speed(r.session.s, 1000000 + i * 10000u,
                         i % 2 == 0 ? 3000 : 1000);
        }
        const uint64_t lost = r.state().conditions_lost;
        EXPECT_GT(lost, 0u);
        if (through_the_ring) {
            /* A drained ring takes what fits of the carry, 40 bytes per
             * event after a record's head: four drains empty a full one */
            ecuio_recorder_test_hold(r.recorder, 0);
            for (int i = 0; i < 4; ++i) {
                r.drained();
                engine_speed(r.session.s, 8000000 + i * 10000u,
                             i % 2 == 0 ? 3000 : 1000);
            }
            r.drained();
            EXPECT_EQ(r.state().conditions_lost, lost) << "nothing more lost";
        }
        r.close();

        const std::vector<Cond> lines = cond_lines(r.folder);
        EXPECT_EQ(lines.size() + lost, r.events.all.size());
        EXPECT_GE(lines.size(), 256u) << "the carry, and what the ring held";
        EXPECT_EQ(lost_lines(r.folder),
                  std::vector<std::string>{lost_line(lost)});
        EXPECT_EQ(events_lost_read(r.folder), lost)
            << "the reader counts what the recorder said";
        size_t at = 0;
        for (const Cond& c : lines) {
            while (at < r.events.all.size() &&
                   r.events.all[at].timestamp_us != c.t_us) {
                ++at;
            }
            ASSERT_LT(at, r.events.all.size())
                << "a line at " << c.t_us
                << " the session never gave, or out of order";
        }
    }
}

TEST(ConditionRecordingTest, EachRunOfLostEventsHasADropLineOfItsOwn)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    uint64_t t = 1000000;
    bool high = false;
    const auto flood = [&](int frames) {
        for (int i = 0; i < frames; ++i, t += 10000) {
            high = !high;
            engine_speed(r.session.s, t, high ? 3000 : 1000);
        }
    };
    /* A run, a part of the carry taken in, a second run, then the rest */
    ecuio_recorder_test_hold(r.recorder, 1);
    flood(600);
    const uint64_t first = r.state().conditions_lost;
    ASSERT_GT(first, 0u);
    ecuio_recorder_test_hold(r.recorder, 0);
    r.drained();
    flood(1);
    ecuio_recorder_test_hold(r.recorder, 1);
    /* A pass under way when the hold was set drains the ring once more:
     * the flood waits it out, or its room splits the run in two */
    r.drained();
    flood(600);
    const uint64_t second = r.state().conditions_lost - first;
    ASSERT_GT(second, 0u);
    ecuio_recorder_test_hold(r.recorder, 0);
    for (int i = 0; i < 4; ++i) {
        r.drained();
        flood(1);
    }
    r.close();

    EXPECT_EQ(lost_lines(r.folder),
              (std::vector<std::string>{lost_line(first), lost_line(second)}));
    EXPECT_EQ(events_lost_read(r.folder), first + second);
    EXPECT_EQ(cond_lines(r.folder).size() + first + second,
              r.events.all.size());
}

TEST(ConditionRecordingTest, ALinkTransitionFollowsTheEventsCarriedBeforeIt)
{
    Recorded r(set_of(on_rpm("flip", R"("above":2000)")), 4096);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    for (int i = 0; i < 200; ++i) {
        engine_speed(r.session.s, 1000000 + i * 10000u,
                     i % 2 == 0 ? 3000 : 1000);
    }
    ecuio_recorder_test_hold(r.recorder, 0);
    r.drained();
    /* The worker's next record is a link transition, after them all */
    ecuio_record_link_state(r.session.s->record_link.get(),
                            ECUIO_ERR_VEHICLE_TIMEOUT, 0);
    r.close();

    size_t last_cond = 0;
    size_t link = 0;
    const std::vector<std::string> log = lines_of(r.folder / "events.log");
    for (size_t i = 0; i < log.size(); ++i) {
        const size_t at = log[i].find("  cond  ");
        last_cond = at != std::string::npos && at < 24 ? i : last_cond;
        link =
            log[i].find("  link  vehicle not responding") != std::string::npos
                ? i
                : link;
    }
    ASSERT_NE(link, 0u) << "the transition is written";
    EXPECT_EQ(cond_lines(r.folder).size(), r.events.all.size());
    EXPECT_GT(link, last_cond)
        << "a cond line carried from before the transition comes after it";
}

TEST(ConditionRecordingTest, AFiringOfAConditionWithMarkIsAMarker)
{
    Recorded r(set_of(R"({"key":"high","name":"High revs","mark":true,)"
                      R"("when":{"signal":["obd2.rpm"],"unit":"RPM",)"
                      R"("above":2000}},)" +
                      on_rpm("plain", R"("above":2500)")));
    ASSERT_NE(r.recorder, nullptr);
    engine_speed(r.session.s, 1000000, 3000); /* ON before the recording */
    r.record();
    engine_speed(r.session.s, 2000000, 3000); /* its held line */
    engine_speed(r.session.s, 3000000, 1000);
    engine_speed(r.session.s, 4000000, 3000);
    engine_speed(r.session.s, 5000000, 1000);
    engine_speed(r.session.s, 6000000, 3000);
    r.close();

    std::vector<uint64_t> fired;
    for (const Cond& c : cond_lines(r.folder)) {
        if (c.key == "high" && c.flags == "fired") {
            fired.push_back(c.host_us);
        }
    }
    ASSERT_EQ(fired.size(), 2u);
    const std::vector<std::string> markers = lines_of(r.folder / "markers.csv");
    ASSERT_EQ(markers.size(), 3u) << "the header and a row per firing";
    EXPECT_EQ(markers[0], "host_us,label");
    for (size_t i = 0; i < fired.size(); ++i) {
        EXPECT_EQ(markers[i + 1], std::to_string(fired[i]) + ",High revs");
    }
}

TEST(ConditionRecordingTest,
     DispatchWithARecorderAndTheLargestSetAllocatesNothing)
{
    std::string all;
    for (int i = 0; i < static_cast<int>(ECUIO_CONDITION_MAX_CONDITIONS); ++i) {
        const std::string rpm = R"({"signal":["obd2.rpm"],"unit":"RPM",)";
        const std::string four =
            rpm + R"("above":)" + std::to_string(1000 + 50 * i) + "}," + rpm +
            R"("below":9000,"clear":9100},)" +
            R"({"signal":["obd2.throttle"],"unit":"%","above":)" +
            std::to_string(i) + "}," + rpm + R"("above":500})";
        all += std::string(i == 0 ? "" : ",") + R"({"key":"c)" +
               std::to_string(i) + R"(","name":"C)" + std::to_string(i) +
               R"(","mark":true,"when":{"all":[)" + four +
               R"(]},"clear_for":0.5})";
    }
    /* A small ring held: frames dropped, events carried, then lost */
    Recorded r(set_of(all), 4096);
    ASSERT_NE(r.recorder, nullptr);
    /* Most conditions ON before the recording: its first frame holds them */
    for (int f = 0; f < 50; ++f) {
        const uint64_t t = 1000000 + f * 110000u;
        if (f % 2 == 0) {
            engine_speed(r.session.s, t, 5000);
        } else {
            throttle(r.session.s, t, 255);
        }
    }
    r.record();
    ecuio_recorder_test_hold(r.recorder, 1);
    r.events.all.reserve(1u << 20); /* the case's own callback's room */
    const unsigned long before = news_on_this_thread();
    for (int f = 0; f < 2000; ++f) {
        const uint64_t t = 10000000 + f * 110000u;
        if (f % 3 == 2) {
            throttle(r.session.s, t, static_cast<uint8_t>(f * 7 % 256));
        } else {
            engine_speed(r.session.s, t, static_cast<unsigned>(f * 977 % 9000));
        }
    }
    EXPECT_EQ(news_on_this_thread() - before, 0ul);
    EXPECT_GT(r.state().conditions_lost, 0u) << "the carry overflowed";
    r.close();
    bool held = false;
    for (const Cond& c : cond_lines(r.folder)) {
        held = held || c.flags.find("held") != std::string::npos;
    }
    EXPECT_TRUE(held) << "the counted frames began with held states";
}

/* The reader gives back each event the recording holds (ADR-0063 section
 * 8): the held state, then every change the session gave, field for
 * field, through signals.csv and through the frames alike, and the copy
 * matching its line */
TEST(ConditionRecordingTest, TheReaderGivesEachEventTheSessionGave)
{
    const std::string json =
        set_of(R"({"key":"high","name":"High revs","mark":true,)"
               R"("when":{"signal":["obd2.rpm"],"unit":"RPM",)"
               R"("above":2000}},)" +
               on_rpm("low", R"("below":1500)"));
    Recorded r(json);
    ASSERT_NE(r.recorder, nullptr);
    engine_speed(r.session.s, 1000000, 3000); /* high ON before it */
    r.record();
    engine_speed(r.session.s, 2000000, 3100);
    engine_speed(r.session.s, 3000000, 1000);
    engine_speed(r.session.s, 4000000, 3200);
    r.close();

    std::vector<ecuio_condition_event_t> given;
    for (const ecuio_condition_event_t& e : r.events.all) {
        if (e.timestamp_us >= 2000000) {
            given.push_back(e);
        }
    }
    ASSERT_EQ(given.size(), 4u) << "two changes at 3 s and two at 4 s";
    const std::vector<Cond> lines = cond_lines(r.folder);
    ASSERT_EQ(lines.size(), given.size() + 1) << "a held line first";
    const std::string folder = r.folder.generic_u8string();

    for (int frames = 0; frames < 2; ++frames) {
        ecuio_recording_t* rec = nullptr;
        ASSERT_EQ(frames == 0 ? ecuio_recording_open(folder.c_str(), nullptr,
                                                     nullptr, &rec)
                              : ecuio_recording_open_frames(
                                    folder.c_str(), ecuio_obd2_signal_set(),
                                    nullptr, nullptr, &rec),
                  ECUIO_OK)
            << ecuio_last_error_detail();
        ecuio_recording_info_t info{};
        ASSERT_EQ(ecuio_recording_get_info(rec, &info), ECUIO_OK);
        EXPECT_EQ(info.condition_events, lines.size()) << frames;
        ecuio_recording_conditions_t c{};
        ASSERT_EQ(ecuio_recording_get_conditions(rec, &c), ECUIO_OK);
        EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED |
                               ECUIO_RECORDING_CONDITIONS_PRESENT |
                               ECUIO_RECORDING_CONDITIONS_MATCH)
            << frames;
        EXPECT_EQ(c.length, json.size());
        EXPECT_EQ(std::string(c.sha256),
                  ecuio_sha256::hex_of(json.data(), json.size()));
        EXPECT_EQ(c.lines_skipped, 0u);

        ecuio_recording_condition_event_t e{};
        ASSERT_EQ(ecuio_recording_get_condition_event(rec, 0, &e), ECUIO_OK);
        EXPECT_STREQ(e.key, "high");
        EXPECT_EQ(e.state, ECUIO_CONDITION_ON);
        EXPECT_EQ(e.flags, ECUIO_CONDITION_HELD);
        EXPECT_EQ(e.timestamp_us, 2000000u);
        EXPECT_EQ(e.host_us, lines[0].host_us);
        for (size_t i = 0; i < given.size(); ++i) {
            const ecuio_condition_event_t& g = given[i];
            ASSERT_EQ(ecuio_recording_get_condition_event(rec, i + 1, &e),
                      ECUIO_OK);
            EXPECT_STREQ(e.key, g.condition == 0 ? "high" : "low") << i;
            EXPECT_EQ(e.state, g.state) << i;
            EXPECT_EQ(e.flags, g.flags) << i;
            EXPECT_EQ(std::memcmp(&e.value, &g.value, sizeof e.value), 0)
                << "the value's bits, " << i;
            EXPECT_EQ(e.term, g.term) << i;
            EXPECT_EQ(e.timestamp_us, g.timestamp_us) << i;
            EXPECT_EQ(e.host_us, lines[i + 1].host_us) << i;
        }
        EXPECT_EQ(ecuio_recording_get_condition_event(rec, lines.size(), &e),
                  ECUIO_ERR_INVALID_ARGUMENT);
        ecuio_recording_close(rec);
    }
}

/* A folder export keeps the set and its events (ADR-0063 section 8):
 * conditions.json and its header line, each cond line as written, raw or
 * scrubbed; a range opens with a held line for each condition ON at its
 * start, and the copy reads back as a recording of its own */
TEST(ConditionRecordingTest, AFolderExportKeepsTheSetAndARangeOpensWithTheHeld)
{
    const std::string json =
        set_of(on_rpm("high", R"("above":2000)", R"(,"mark":true)") + "," +
               on_rpm("low", R"("below":1500)"));
    Recorded r(json);
    ASSERT_NE(r.recorder, nullptr);
    r.record();
    /* 20 ms apart, so a range can start between two frames' lines */
    const int rpms[] = {3000, 1000, 3100, 1200};
    for (int i = 0; i < 4; ++i) {
        engine_speed(r.session.s, 1000000u * (i + 1), rpms[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    r.close();
    const std::vector<std::string> source = kind_lines(r.folder, "cond");
    const std::vector<Cond> parsed = cond_lines(r.folder);
    ASSERT_EQ(source.size(), 8u) << "two at each frame (low's first is off)";
    const std::string sha = ecuio_sha256::hex_of(json.data(), json.size());

    auto exported = [&](const char* name, uint32_t flags, uint64_t from_us) {
        const std::string in = r.folder.generic_u8string();
        const std::string out = (r.root.path / name).generic_u8string();
        ecuio_export_options_t o{};
        o.format = ECUIO_EXPORT_FOLDER;
        o.flags = flags;
        o.from_us = from_us;
        o.folder_utf8 = in.c_str();
        o.out_path_utf8 = out.c_str();
        ecuio_export_report_t report{};
        EXPECT_EQ(ecuio_export(&o, &report), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(report.files_left_out, 0u) << name;
        return r.root.path / name;
    };

    for (const uint32_t flags : {0u, static_cast<uint32_t>(ECUIO_EXPORT_RAW)}) {
        const fs::path copy =
            exported(flags == 0 ? "scrubbed" : "raw", flags, 0);
        EXPECT_EQ(read_bytes(copy / "conditions.json"), json) << flags;
        const std::vector<std::string> head = lines_of(copy / "events.log");
        EXPECT_NE(std::find(head.begin(), head.end(),
                            "conditions  conditions.json," + sha),
                  head.end())
            << flags;
        EXPECT_EQ(kind_lines(copy, "cond"), source) << flags;
        EXPECT_EQ(lines_of(copy / "markers.csv"),
                  lines_of(r.folder / "markers.csv"))
            << flags;
    }

    /* From just after the second frame's changes: low stands ON there */
    const Cond& low_on = parsed[3];
    ASSERT_EQ(low_on.key + "," + low_on.state, "low,on");
    const uint64_t from = low_on.host_us + 1;
    const fs::path ranged = exported("ranged", 0, from);
    const std::vector<Cond> got = cond_lines(ranged);
    ASSERT_EQ(got.size(), 5u) << "a held line, then the last four";
    EXPECT_EQ(got[0].key, "low");
    EXPECT_EQ(got[0].state, "on");
    EXPECT_EQ(got[0].flags, "held");
    EXPECT_EQ(got[0].value, low_on.value);
    EXPECT_EQ(got[0].term, low_on.term);
    EXPECT_EQ(got[0].host_us, from);
    EXPECT_EQ(got[0].t_us, low_on.t_us + 1);
    const std::vector<std::string> kept = kind_lines(ranged, "cond");
    for (size_t i = 1; i < kept.size(); ++i) {
        EXPECT_EQ(kept[i], source[i + 3]) << i;
    }

    ecuio_recording_t* rec = nullptr;
    const std::string folder = ranged.generic_u8string();
    ASSERT_EQ(ecuio_recording_open(folder.c_str(), nullptr, nullptr, &rec),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_recording_conditions_t c{};
    ASSERT_EQ(ecuio_recording_get_conditions(rec, &c), ECUIO_OK);
    EXPECT_EQ(c.flags, ECUIO_RECORDING_CONDITIONS_NAMED |
                           ECUIO_RECORDING_CONDITIONS_PRESENT |
                           ECUIO_RECORDING_CONDITIONS_MATCH);
    ecuio_recording_info_t info{};
    ASSERT_EQ(ecuio_recording_get_info(rec, &info), ECUIO_OK);
    EXPECT_EQ(info.condition_events, 5u);
    ecuio_recording_condition_event_t e{};
    ASSERT_EQ(ecuio_recording_get_condition_event(rec, 0, &e), ECUIO_OK);
    EXPECT_STREQ(e.key, "low");
    EXPECT_EQ(e.flags, ECUIO_CONDITION_HELD);
    EXPECT_EQ(e.host_us, from);
    ecuio_recording_close(rec);
}
