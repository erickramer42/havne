// File/layer: tests/test_cli_signals.cpp | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#include "test_cli_support.h"

// A plugin whose data files include one of format 2 is warned of a
// requires.core below 0.8.0 and of no other; the gauge's warning is its own
TEST(CliPluginTest, AFormat2FileWantsTheCoreThatReadsIt)
{
    struct Version {
        uint16_t part[3];
    };
    const auto older = [](Version v, bool uses_format2) {
        return cli::format2_needs_newer_core(v.part, uses_format2);
    };
    EXPECT_FALSE(older({{0, 8, 0}}, true));
    EXPECT_FALSE(older({{0, 8, 1}}, true));
    EXPECT_FALSE(older({{1, 0, 0}}, true));
    EXPECT_TRUE(older({{0, 7, 9}}, true));
    EXPECT_TRUE(older({{0, 1, 0}}, true));
    EXPECT_FALSE(older({{0, 1, 0}}, false));

    char buf[1024];
    cli::Line l(buf, sizeof buf);
    const uint16_t declared[3] = {0, 7, 3};
    cli::requires_core_line(l, declared, cli::kFormat2Core,
                            "\"havne_signals\": 2");
    EXPECT_EQ(text_of(l),
              "{\"type\":\"requires_core\",\"declared\":\"0.7.3\",\"needs\":"
              "\"0.8.0\",\"warning\":\"the data files use "
              "\\\"havne_signals\\\": 2, which core reads from 0.8.0 on: "
              "raise the manifest's \\\"requires\\\" core to 0.8.0, so an "
              "older Havne names that version instead of refusing a signal "
              "file\"}\n");
}

// ---- A replay's own signal file (cli/recording_signals.h, ADR-0051
// section 4) ----

/* The header's profile and signal file lines, up to its blank line; a
 * hash that isn't 64 lowercase hex digits names no file */
TEST(CliRecordingSignalsTest, TheHeaderNamesTheProfileAndTheFile)
{
    const std::string sha(64, 'a');
    cli::RecordedSignals r;
    EXPECT_TRUE(r.take("ecuio session recording, format 3 (ADR-0049)"));
    EXPECT_TRUE(r.take("profile  user.extras\r"));
    EXPECT_TRUE(r.take("signal  A,number,,0,0,0"));
    EXPECT_TRUE(r.take("signal file  signals.json," + sha));
    EXPECT_FALSE(r.take(""));
    EXPECT_FALSE(r.take("profile  after.the.header"));
    EXPECT_EQ(r.profile, "user.extras");
    EXPECT_EQ(r.sha256, sha);
    EXPECT_TRUE(r.names_file());

    cli::RecordedSignals upper;
    upper.take("profile  user.extras");
    upper.take("signal file  signals.json," + std::string(64, 'A'));
    EXPECT_FALSE(upper.names_file()) << "the recorder writes lower case";
    cli::RecordedSignals short_hash;
    short_hash.take("profile  user.extras");
    short_hash.take("signal file  signals.json," + std::string(63, 'a'));
    EXPECT_FALSE(short_hash.names_file());
    cli::RecordedSignals none;
    none.take("profile  obd2");
    EXPECT_FALSE(none.names_file());
    EXPECT_EQ(none.conditions_sha256, "");
}

/* The conditions the recording judged: their copy's line and hash, by the
 * signal file's rule, and only in the header */
TEST(CliRecordingSignalsTest, TheHeaderNamesTheConditionsCopy)
{
    const std::string sha(64, 'c');
    cli::RecordedSignals r;
    r.take("ecuio session recording, format 3 (ADR-0049)");
    r.take("profile  obd2");
    r.take("conditions  conditions.json," + sha + "\r");
    EXPECT_EQ(r.conditions_sha256, sha);
    EXPECT_EQ(r.sha256, "") << "no signal file named";
    EXPECT_FALSE(r.names_file());
    r.take("");
    r.take("conditions  conditions.json," + std::string(64, 'd'));
    EXPECT_EQ(r.conditions_sha256, sha) << "past the header";

    const std::string bad[] = {
        "conditions  conditions.json," + std::string(64, 'C'),
        "conditions  conditions.json," + std::string(63, 'c'),
        "conditions  conditions.json," + std::string(65, 'c'),
        "conditions  other.json," + sha,
        "conditions  conditions.json " + sha,
    };
    for (const std::string& line : bad) {
        cli::RecordedSignals b;
        b.take(line);
        EXPECT_EQ(b.conditions_sha256, "") << line;
    }
}

/* The folder: the path up to the query, a frames.csv's own folder, any
 * scheme's case; another scheme none */
TEST(CliRecordingSignalsTest, AReplayUrisFolder)
{
    std::string folder;
    ASSERT_TRUE(
        cli::replay_folder("replay://C:/r/20261106-0900?speed=max", &folder));
    EXPECT_EQ(folder, "C:/r/20261106-0900");
    ASSERT_TRUE(cli::replay_folder("REPLAY:///data/r/frames.csv", &folder));
    EXPECT_EQ(folder, "/data/r");
    // Another .csv is a frames file of its own: its folder's events.log
    // and signals.json aren't its, as core's source_frame_flags says
    EXPECT_FALSE(cli::replay_folder("replay:///data/r/other.csv", &folder));
    EXPECT_FALSE(cli::replay_folder("replay:///data/r/frames.CSV", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://frames.csv", &folder));
    ASSERT_TRUE(cli::replay_folder("replay:///data/r/", &folder));
    EXPECT_EQ(folder, "/data/r");
    EXPECT_FALSE(cli::replay_folder("mock://demo", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://", &folder));
    EXPECT_FALSE(cli::replay_folder("replay://?profile=obd2", &folder));
}

/* One profile=, the rest of the query kept in order; the copy's own
 * name from the hash */
TEST(CliRecordingSignalsTest, TheUriWithAnotherProfile)
{
    EXPECT_EQ(cli::with_profile("replay://f", "rec.0123"),
              "replay://f?profile=rec.0123");
    EXPECT_EQ(cli::with_profile("replay://f?profile=user.a&speed=max", "rec.1"),
              "replay://f?speed=max&profile=rec.1");
    EXPECT_EQ(
        cli::with_profile("replay://f?speed=2&profile=a&loop=1&profile=b", "c"),
        "replay://f?speed=2&loop=1&profile=c");
    // An empty pair is kept, so core refuses the query as it would have
    EXPECT_EQ(cli::with_profile("replay://f?", "c"), "replay://f?&profile=c");
    EXPECT_EQ(cli::with_profile("replay://f?speed=2&&loop=1", "c"),
              "replay://f?speed=2&&loop=1&profile=c");
    EXPECT_EQ(cli::with_profile("replay://f?profile=a", "c"),
              "replay://f?profile=c");
    EXPECT_EQ(cli::own_profile(std::string(16, '0') + std::string(48, 'f')),
              "rec.0000000000000000");
}

// ---- The signals an ECU refuses (cli/refused_signals.h, ADR-0051) ----

namespace {

// A def keyed at the reply's start: key's bytes, first most significant
ecuio_signal_def_t keyed(uint16_t id, uint32_t rx, uint8_t length, uint32_t key)
{
    ecuio_signal_def_t d{};
    d.id = id;
    d.message_id = rx;
    d.mux_offset = 0;
    d.mux_length = length;
    d.mux_key = key;
    return d;
}

std::vector<uint16_t> ids_of(const std::vector<cli::RefusedSignal>& r)
{
    std::vector<uint16_t> ids;
    for (const cli::RefusedSignal& s : r) {
        ids.push_back(s.id);
    }
    return ids;
}

} // namespace

/* A refused request leaves its own signals without values: the defs on
 * its channel's reply ID keyed by its reply (62 <DID>, 41 <PID>), each
 * once, in the set's order; another ECU's, another key's, an unkeyed or
 * a computed def keep theirs, and a PID its bitmap left out is
 * unsupported, not refused */
TEST(CliRefusedTest, ARefusedRequestIsItsOwnSignalsAlone)
{
    std::vector<ecuio_signal_def_t> defs = {
        keyed(1, 0x7E8, 3, 0x62F1FE), // refused
        keyed(2, 0x7E8, 3, 0x621E1C), // answered
        keyed(3, 0x7E9, 3, 0x62F1FE), // the same DID on another ECU
        keyed(4, 0x7E8, 2, 0x410C),   // refused by its bitmap
        keyed(5, 0x7E8, 0, 0),        // unkeyed on the ID
        keyed(6, 0x7E8, 3, 0x62F1FE), // a second field of the refused reply
        keyed(7, 0x7E8, 2, 0x410D),   // refused by a negative reply
        keyed(8, 0x7E8, 3, 0x62F1FF), // refused for its access
    };
    ecuio_signal_def_t computed = keyed(9, 0, 0, 0);
    computed.flags = ECUIO_DEF_FLAG_COMPUTED;
    defs.push_back(computed);
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();
    const ecuio_channel_support_t reports[2] = {
        report(0x7E8, {dropped(0x22, 0xF1FE, 3, ECUIO_DROP_NOT_SUPPORTED, 0x31),
                       dropped(0x01, 0x0C, 2, ECUIO_DROP_BITMAP, 0),
                       dropped(0x01, 0x0D, 2, ECUIO_DROP_SERVICE, 0x11),
                       dropped(0x22, 0xF1FF, 3, ECUIO_DROP_ACCESS, 0x33)}),
        report(0x7E9, {}),
    };
    const std::vector<cli::RefusedSignal> r =
        cli::refused_signals(&set, reports, 2);
    EXPECT_EQ(ids_of(r), (std::vector<uint16_t>{1, 6, 7, 8}));
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0].request[0], 0x22);
    EXPECT_EQ(r[0].request[1], 0xF1);
    EXPECT_EQ(r[0].request[2], 0xFE);
    EXPECT_EQ(r[0].length, 3);
    EXPECT_EQ(r[0].nrc, 0x31);
    EXPECT_EQ(r[2].nrc, 0x11);
    EXPECT_EQ(r[3].nrc, 0x33);
    EXPECT_EQ(ids_of(cli::refused_signals(&set, reports, 1)),
              (std::vector<uint16_t>{1, 6, 7, 8}))
        << "the channels counted";
    EXPECT_EQ(ids_of(cli::refused_signals(&set, reports + 1, 1)),
              std::vector<uint16_t>{})
        << "the other ECU refused nothing";
}

/* A report that has learned nothing refuses nothing, only the drops the
 * report lists count, and no set or no reports refuse nothing */
TEST(CliRefusedTest, OnlyALearnedReportsListedDropsRefuse)
{
    std::vector<ecuio_signal_def_t> defs;
    for (uint16_t did = 0; did < 10; ++did) {
        defs.push_back(
            keyed(static_cast<uint16_t>(100 + did), 0x7E8, 3, 0x62F100u + did));
    }
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();
    ecuio_channel_support_t s = report(0x7E8, {});
    for (uint16_t did = 0; did < ECUIO_SUPPORT_MAX_DROPPED; ++did) {
        s.dropped_list[did] = dropped(0x22, static_cast<uint16_t>(0xF100 + did),
                                      3, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    }
    s.dropped = 10; // two past the list: core keeps the first eight
    EXPECT_EQ(cli::refused_signals(&set, &s, 1).size(),
              size_t{ECUIO_SUPPORT_MAX_DROPPED});
    s.dropped = 3;
    EXPECT_EQ(ids_of(cli::refused_signals(&set, &s, 1)),
              (std::vector<uint16_t>{100, 101, 102}))
        << "a listed slot past the count isn't a drop";
    s.generation = 0;
    EXPECT_TRUE(cli::refused_signals(&set, &s, 1).empty());
    s.generation = 1;
    EXPECT_TRUE(cli::refused_signals(nullptr, &s, 1).empty());
    EXPECT_TRUE(cli::refused_signals(&set, nullptr, 1).empty());
}

/* The refused line: each signal's id, its request and the reply in
 * frames.csv's form, the code by name, null for one with none; an
 * emptied list is [] */
TEST(CliRefusedTest, TheRefusedLineNamesEachSignalsReply)
{
    cli::RefusedSignal a;
    a.id = 30001;
    a.request[0] = 0x22;
    a.request[1] = 0xF1;
    a.request[2] = 0xFE;
    a.length = 3;
    a.nrc = 0x31;
    cli::RefusedSignal b = a;
    b.id = 12;
    b.request[0] = 0x01;
    b.request[1] = 0x0D;
    b.length = 2;
    b.nrc = 0x99;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::refused_line(l, {a, b});
    EXPECT_EQ(
        std::string(l.data(), l.size()),
        "{\"type\":\"refused\",\"signals\":[{\"id\":30001,\"request\":\"22 F1 "
        "FE\","
        "\"reply\":\"7F 22 31\",\"nrc\":\"requestOutOfRange\"},{\"id\":12,"
        "\"request\":\"01 0D\",\"reply\":\"7F 01 99\",\"nrc\":null}]}");
    cli::Line e(buf, sizeof buf);
    cli::refused_line(e, {});
    EXPECT_EQ(std::string(e.data(), e.size()),
              "{\"type\":\"refused\",\"signals\":[]}");
    EXPECT_LE(std::string(l.data(), l.size()).size(),
              cli::refused_line_bytes(2));
}

/* Past a report's list, refused_unlisted counts the refusals: the drops
 * past the list less the requests the bitmaps left out (one a Service 01
 * PID among the set's defs on the channel, a PID's second def not
 * counted again). A list that ends in a bitmap drop, or has room, named
 * every refusal; a report that learned nothing counts none. */
TEST(CliRefusedTest, RefusalsPastTheListAreCounted)
{
    std::vector<ecuio_signal_def_t> defs;
    for (uint16_t did = 0; did < 12; ++did) {
        defs.push_back(
            keyed(static_cast<uint16_t>(100 + did), 0x7E8, 3, 0x62F100u + did));
    }
    defs.push_back(keyed(200, 0x7E8, 2, 0x4104)); // left out by its bitmap
    defs.push_back(keyed(201, 0x7E8, 2, 0x4104)); // its second field
    defs.push_back(keyed(202, 0x7E8, 2, 0x4105)); // left out by its bitmap
    defs.push_back(keyed(203, 0x7E8, 2, 0x410C)); // supported
    defs.push_back(keyed(204, 0x7E8, 2, 0x4161)); // its bitmap unanswered
    defs.push_back(keyed(205, 0x7E9, 2, 0x4106)); // another ECU's
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = static_cast<uint32_t>(defs.size());
    set.defs = defs.data();

    ecuio_channel_support_t s = report(0x7E8, {});
    s.ranges = 0x1; // the bitmap at 0x00 answered: PIDs 0x01 to 0x20
    s.pid_bitmap[(0x0C - 1) / 8] |= 0x80u >> ((0x0C - 1) % 8);
    for (uint16_t did = 0; did < ECUIO_SUPPORT_MAX_DROPPED; ++did) {
        s.dropped_list[did] = dropped(0x22, static_cast<uint16_t>(0xF100 + did),
                                      3, ECUIO_DROP_NOT_SUPPORTED, 0x31);
    }
    // the twelve DIDs refused and two PIDs left out: four refusals past
    s.dropped = 12 + 2;
    std::vector<cli::RefusedMore> more = cli::refused_unlisted(&set, &s, 1);
    ASSERT_EQ(more.size(), 1u);
    EXPECT_EQ(more[0].rx_id, 0x7E8u);
    EXPECT_EQ(more[0].count, 4u);

    s.dropped = 8 + 2; // only the bitmap's two past the list
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty());
    s.dropped = 12 + 2;
    s.dropped_list[ECUIO_SUPPORT_MAX_DROPPED - 1] =
        dropped(0x01, 0x04, 2, ECUIO_DROP_BITMAP, 0);
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty())
        << "a list ending in a bitmap drop named every refusal";
    s.dropped_list[ECUIO_SUPPORT_MAX_DROPPED - 1] =
        dropped(0x22, 0xF107, 3, ECUIO_DROP_ACCESS, 0x33);
    s.generation = 0;
    EXPECT_TRUE(cli::refused_unlisted(&set, &s, 1).empty());
    s.generation = 1;
    EXPECT_TRUE(cli::refused_unlisted(nullptr, &s, 1).empty());
    EXPECT_EQ(cli::refused_unlisted(&set, &s, 1).size(), 1u);

    cli::RefusedSignal a;
    a.id = 100;
    a.request[0] = 0x22;
    a.request[1] = 0xF1;
    a.request[2] = 0x00;
    a.length = 3;
    a.nrc = 0x31;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::refused_line(l, {a}, more);
    const std::string line(l.data(), l.size());
    EXPECT_EQ(line,
              "{\"type\":\"refused\",\"signals\":[{\"id\":100,\"request\":"
              "\"22 F1 00\",\"reply\":\"7F 22 31\",\"nrc\":"
              "\"requestOutOfRange\"}],\"more\":[{\"ecu\":\"0x7E8\","
              "\"refused\":4}]}");
    EXPECT_LE(line.size(), cli::refused_line_bytes(1, 1));
}

/* The code's names, ISO 14229-1's, and "" past them */
TEST(CliRefusedTest, ANegativeCodeHasItsStandardName)
{
    EXPECT_STREQ(cli::nrc_name(0x11), "serviceNotSupported");
    EXPECT_STREQ(cli::nrc_name(0x12), "subFunctionNotSupported");
    EXPECT_STREQ(cli::nrc_name(0x22), "conditionsNotCorrect");
    EXPECT_STREQ(cli::nrc_name(0x31), "requestOutOfRange");
    EXPECT_STREQ(cli::nrc_name(0x33), "securityAccessDenied");
    EXPECT_STREQ(cli::nrc_name(0x36), "exceedNumberOfAttempts");
    EXPECT_STREQ(cli::nrc_name(0x7F), "serviceNotSupportedInActiveSession");
    EXPECT_STREQ(cli::nrc_name(0x00), "");
    EXPECT_STREQ(cli::nrc_name(0x99), "");
}

/* ---------- havne-cli clear (ADR-0038's appendix of 2026-11-06) ---------- */

namespace {

std::string clear_text(const ecuio_channel_diagnostics_t& d,
                       const char* word = nullptr)
{
    char buf[512];
    cli::Line l(buf, sizeof buf);
    cli::clear_line(l, d, word);
    return std::string(l.data(), l.size());
}

/* A mock session started, destroyed at the end */
struct MockSession {
    ecuio_session_t* s = nullptr;
    explicit MockSession(const char* uri)
    {
        EXPECT_EQ(ecuio_session_create(uri, &s), ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(ecuio_session_start(s), ECUIO_OK);
    }
    ~MockSession() { ecuio_session_destroy(s); }
    MockSession(const MockSession&) = delete;
    MockSession& operator=(const MockSession&) = delete;
};

} // namespace

TEST(CliClearTest, TheOutcomesInWords)
{
    const char* const words[] = {
        "none",           "cleared",        "refused",
        "unanswered",     "engine-running", "no-engine-speed",
        "report-changed", "moving",         "no-vehicle-speed"};
    for (uint8_t i = 0; i < 9; ++i) {
        EXPECT_STREQ(cli::clear_outcome_word(i), words[i]) << int{i};
    }
    EXPECT_STREQ(cli::clear_outcome_word(9), "unknown");
    EXPECT_TRUE(cli::clear_reads_again(ECUIO_CLEAR_CLEARED));
    EXPECT_TRUE(cli::clear_reads_again(ECUIO_CLEAR_UNANSWERED));
    EXPECT_FALSE(cli::clear_reads_again(ECUIO_CLEAR_REFUSED));
    EXPECT_FALSE(cli::clear_reads_again(ECUIO_CLEAR_ENGINE_RUNNING));
}

TEST(CliClearTest, TheLineNamesTheReplyAndWhatWasRead)
{
    ecuio_channel_diagnostics_t d{};
    d.rx_id = 0x7E8;
    d.clear = ECUIO_CLEAR_REFUSED;
    d.clear_nrc = 0x22;
    d.clear_rpm = 812;
    d.flags = ECUIO_DIAG_CLEAR_RPM;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"refused\",\"reply\":\"7F 04 22\","
                             "\"rpm\":812,\"kmh\":null}");
    d.clear = ECUIO_CLEAR_NO_ENGINE_SPEED;
    d.clear_nrc = 0x12;
    d.flags = 0;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"no-engine-speed\","
                             "\"reply\":\"7F 01 12\",\"rpm\":null,"
                             "\"kmh\":null}")
        << "01 0C's refusal";
    d.clear = ECUIO_CLEAR_CLEARED;
    d.clear_nrc = 0;
    d.clear_rpm = 0;
    d.clear_kmh = 30;
    d.flags = ECUIO_DIAG_CLEAR_RPM | ECUIO_DIAG_CLEAR_KMH;
    EXPECT_EQ(clear_text(d), "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                             "\"outcome\":\"cleared\",\"rpm\":0,\"kmh\":30}")
        << "a 0 read is a 0, not a null";
    d.clear_nrc = 0x22;
    EXPECT_EQ(clear_text(d, "not-read"),
              "{\"type\":\"clear\",\"rx\":\"0x7E8\",\"outcome\":\"not-read\","
              "\"rpm\":null,\"kmh\":null}")
        << "the run's own word: nothing of the report's";

    cli::ClearResult r;
    r.reason = "not-cleared";
    r.cleared = 1;
    r.not_cleared = 2;
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::clear_end_line(l, r);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"end\",\"reason\":\"not-cleared\",\"cleared\":1,"
              "\"not_cleared\":2}");
}

// The worker takes a clear by moving the generation on (outcome none,
// clearing) and settles it by moving it on again with an outcome
TEST(CliClearTest, AClearSettlesPastItsGeneration)
{
    ecuio_channel_diagnostics_t d{};
    d.generation = 5;
    d.clear = ECUIO_CLEAR_CLEARED; /* an earlier clear's */
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "not taken yet";
    d.generation = 6;
    d.clear = ECUIO_CLEAR_NONE;
    d.flags = ECUIO_DIAG_CLEARING;
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "under way";
    d.generation = 7;
    d.flags = 0;
    EXPECT_FALSE(cli::clear_settled(d, 5)) << "no outcome";
    d.clear = ECUIO_CLEAR_ENGINE_RUNNING;
    EXPECT_TRUE(cli::clear_settled(d, 5));
    d.clear = ECUIO_CLEAR_CLEARED;
    d.generation = 8; /* the read after, started */
    EXPECT_TRUE(cli::clear_settled(d, 5));
}

// The report first, flushed, then the clear, then the read after a 44
TEST(CliClearTest, TheReportComesFirstAndTheReadAfter)
{
    MockSession m("mock://clear");
    ASSERT_NE(m.s, nullptr);
    Written w;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::seconds(10));
    EXPECT_STREQ(r.reason, "cleared");
    EXPECT_EQ(r.cleared, 1u);
    EXPECT_EQ(r.not_cleared, 0u);
    ASSERT_EQ(w.types(), (std::vector<std::string>{"diagnostics", "clear",
                                                   "diagnostics"}));
    EXPECT_NE(w.lines[0].find("\"P0301\""), std::string::npos) << w.lines[0];
    EXPECT_EQ(w.lines[1], "{\"type\":\"clear\",\"rx\":\"0x7E8\","
                          "\"outcome\":\"cleared\",\"rpm\":0,\"kmh\":null}");
    EXPECT_EQ(w.lines[2].find("\"P0301\""), std::string::npos) << w.lines[2];
    EXPECT_GE(w.flushes, 1);
}

// Output that fails before the clear asks none: the report the ECU holds
// is the one nobody kept
TEST(CliClearTest, AFailedOutputAsksNoClear)
{
    MockSession m("mock://clear");
    ASSERT_NE(m.s, nullptr);
    ecuio_channel_diagnostics_t before{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(m.s, 0, &before), ECUIO_OK);
    Written w;
    w.flush_ok = false;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::seconds(10));
    EXPECT_STREQ(r.reason, "output-failed");
    EXPECT_EQ(r.cleared, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ecuio_channel_diagnostics_t after{};
    ASSERT_EQ(ecuio_session_get_channel_diagnostics(m.s, 0, &after), ECUIO_OK);
    EXPECT_EQ(after.generation, before.generation) << "no clear was taken";
    EXPECT_EQ(after.clear, ECUIO_CLEAR_NONE);
    EXPECT_EQ(w.types(), (std::vector<std::string>{"diagnostics"}));
}

// A session that reads no diagnostics has nothing to save: no clear
TEST(CliClearTest, NothingReadIsNothingCleared)
{
    MockSession m("mock://clear?diagnose=0");
    ASSERT_NE(m.s, nullptr);
    Written w;
    const cli::ClearResult r =
        cli::run_clear(m.s, 1, 0, w.io(), std::chrono::milliseconds(300));
    EXPECT_STREQ(r.reason, "nothing-read");
    EXPECT_EQ(r.not_cleared, 1u);
    ASSERT_EQ(w.lines.size(), 1u);
    EXPECT_NE(w.lines[0].find("\"outcome\":\"not-read\""), std::string::npos);
}
