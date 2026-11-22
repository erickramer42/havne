// File/layer: tests/test_health_baseline.cpp | test_core, Qt-free, C++17
// Testing: UNIT: HealthTest, HealthCancelTest (test_core)

#include "test_health_support.h"

namespace {

// ---- The vehicle's baseline (ADR-0049's appendix "a vehicle's
// baseline") ----

/* A baseline's folder, named as the app names a recording's: its stamp
 * and the vehicle's label, which no report may hold */
const char* const kBaseline = "20261101-090000 Daily driver";

/* A baseline's events.log, all the report reads of it: started 2026-11-01
 * at 09:00 UTC, 312.4 s long, an ELM327 session's under obd2 */
std::string baseline_log(const std::string& before = std::string(),
                         const std::string& after = std::string())
{
    return before +
           "ecuio session recording, format 3 (ADR-0049)\n"
           "started  2026-11-01 09:00:00.000 (UTC)\n"
           "utc  2026-11-01T09:00:00.000Z\n"
           "core  0.6.0\n"
           "uri  elm327://COM4?profile=obd2\n"
           "profile  obd2\n"
           "frame flags  complete\n"
           "\n"
           "09:00:01.250  +1.250  link  ok (state 0, 0 episodes)\n" +
           after +
           "09:05:12.400  +312.400  end  0 records dropped, 0 notes "
           "dropped\n";
}

const char* const kBaselineLine =
    "\nBaseline: the vehicle's baseline recording, started "
    "2026-11-01T09:00:00Z, 312.400 s (elm327), profile obd2\n";

/* The report names the baseline by its start, length, backend and
 * profile, in both formats; nothing else in the report changes, and
 * neither its folder's name, its label nor its URI's port is written. A
 * baseline of "" is none, as NULL is. */
TEST(HealthTest, AReportNamesTheBaselineByItsStartNeverItsFolder)
{
    Folder f;
    car_recording(f);
    f.write(std::string(kBaseline) + "/events.log", baseline_log());
    const std::string base = f.utf8(kBaseline);
    const std::string rec = f.utf8("rec");
    const Reported json(f, rec, ECUIO_HEALTH_JSON, 0, 0, base.c_str());
    const Reported text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, base.c_str());
    ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
    ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
    const ecuio_json::Value v = parsed(json.bytes);
    const ecuio_json::Value* b = v.find("baseline");
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(b->type, ecuio_json::Type::object) << json.bytes;
    EXPECT_TRUE(b->find("read")->boolean);
    EXPECT_EQ(b->find("started")->text, "2026-11-01T09:00:00Z");
    EXPECT_EQ(b->find("length_s")->number, 312.4);
    EXPECT_EQ(b->find("profile")->text, "obd2");
    EXPECT_EQ(b->find("backend")->text, "elm327");
    EXPECT_FALSE(b->find("is_source")->boolean);
    EXPECT_EQ(b->find("why"), nullptr);
    EXPECT_EQ(json.r.flags & ECUIO_HEALTH_BASELINE_READ,
              ECUIO_HEALTH_BASELINE_READ);
    EXPECT_NE(text.bytes.find(kBaselineLine), std::string::npos) << text.bytes;
    std::vector<std::string> facts;
    strings_of(*b, facts);
    for (const std::string& s : facts) {
        EXPECT_NE(text.bytes.find(s), std::string::npos)
            << "the text lacks '" << s << "'";
    }
    for (const std::string& out : {json.bytes, text.bytes}) {
        EXPECT_EQ(out.find("Daily"), std::string::npos);
        EXPECT_EQ(out.find("20261101"), std::string::npos);
        EXPECT_EQ(out.find("COM4"), std::string::npos);
    }

    /* Without one, null and no line; the rest the same bytes */
    const Reported plain_json(f, rec, ECUIO_HEALTH_JSON);
    const Reported plain_text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, "");
    ASSERT_EQ(plain_text.status, ECUIO_OK) << plain_text.detail;
    EXPECT_EQ(parsed(plain_json.bytes).find("baseline")->type,
              ecuio_json::Type::null);
    EXPECT_EQ(plain_json.r.flags & ECUIO_HEALTH_BASELINE_READ, 0u);
    EXPECT_EQ(plain_text.bytes.find("Baseline"), std::string::npos);
    std::string without = text.bytes;
    without.replace(without.find(kBaselineLine), std::strlen(kBaselineLine),
                    "\n");
    EXPECT_EQ(without, plain_text.bytes);
}

/* The report's own recording as its baseline is said to be it, and so is
 * a copy of it (the same start); a session's report never is */
TEST(HealthTest, TheReportsOwnRecordingAsItsBaselineIsSaidToBeIt)
{
    Folder f;
    car_recording(f);
    f.write("copy/events.log", read_file(f.path / "rec" / "events.log"));
    const std::string rec = f.utf8("rec");
    for (const std::string& base : {rec, f.utf8("copy")}) {
        const Reported json(f, rec, ECUIO_HEALTH_JSON, 0, 0, base.c_str());
        const Reported text(f, rec, ECUIO_HEALTH_TEXT, 0, 0, base.c_str());
        ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
        EXPECT_TRUE(
            parsed(json.bytes).find("baseline")->find("is_source")->boolean);
        EXPECT_NE(text.bytes.find("\nBaseline: the vehicle's baseline "
                                  "recording, started 2026-11-06T05:30:00Z ("
                                  "elm327), profile obd2: this recording\n"),
                  std::string::npos)
            << text.bytes;
    }

    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://Daily driver", &s), ECUIO_OK);
    const std::string out = f.utf8("session.json");
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    o.utc_us = kUtc;
    o.baseline_folder_utf8 = rec.c_str();
    ecuio_health_report_t r{};
    ASSERT_EQ(ecuio_health_export(&o, &r), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_json::Value v = parsed(read_file(f.path / "session.json"));
    EXPECT_TRUE(v.find("baseline")->find("read")->boolean);
    EXPECT_FALSE(v.find("baseline")->find("is_source")->boolean);
    EXPECT_EQ(r.flags & ECUIO_HEALTH_BASELINE_READ, ECUIO_HEALTH_BASELINE_READ);
    ecuio_session_destroy(s);
}

/* A baseline that can't be read says why in both formats, never its
 * path, and the report is still written */
TEST(HealthTest, ABaselineThatCantBeReadSaysWhyAndTheReportIsWritten)
{
    Folder f;
    car_recording(f);
    f.write("no-log/frames.csv", "host_us,timestamp_us,id,flags,data\n");
    f.write("not-a-recording/events.log", "a shopping list\n");
    const struct {
        const char* folder;
        const char* why;
    } cases[] = {
        {"gone", "its folder is gone: it was moved or deleted"},
        {"no-log", "its folder holds no events.log"},
        {"not-a-recording", "its events.log isn't a recording's"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder);
        const std::string base = f.utf8(c.folder);
        const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 0, 0,
                            base.c_str());
        const Reported text(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 0, 0,
                            base.c_str());
        ASSERT_EQ(json.status, ECUIO_OK) << json.detail;
        ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
        const ecuio_json::Value v = parsed(json.bytes);
        const ecuio_json::Value* b = v.find("baseline");
        EXPECT_FALSE(b->find("read")->boolean);
        EXPECT_EQ(b->find("why")->text, c.why);
        EXPECT_EQ(b->find("started"), nullptr);
        EXPECT_EQ(json.r.flags & ECUIO_HEALTH_BASELINE_READ, 0u);
        EXPECT_NE(text.bytes.find(std::string("\nBaseline: the vehicle's "
                                              "baseline recording can't be "
                                              "read: ") +
                                  c.why + "\n"),
                  std::string::npos)
            << text.bytes;
        for (const std::string& out : {json.bytes, text.bytes}) {
            EXPECT_EQ(out.find(base), std::string::npos);
        }
    }
}

/* events.log is taken as the reader takes it: a byte order mark skipped,
 * format 1 by its first line's ending; a baseline with no utc line has
 * no start, one with no timed line no length; and a line past the
 * reader's room is one line, its rest never read as a timed line of its
 * own */
TEST(HealthTest, ABaselinesEventsLogIsReadAsTheReadersIs)
{
    Folder f;
    car_recording(f);
    const std::string old_header = "Havne 0.3.2 session recording (ADR-0017)\n"
                                   "profile  obd2\n"
                                   "\n";
    /* the reader's room is kRowMax + 1 bytes a read: the line's rest
     * starts with a timed line's shape */
    const std::string note = "09:00:02.000  +2.000  note  ";
    const std::string long_line =
        note +
        std::string(ecuio_recording_rows::kRowMax + 1 - note.size(), 'x') +
        "09:59:59.000  +9999.000  end  0 records dropped, 0 notes dropped\n";
    const struct {
        const char* folder;
        std::string log;
        const char* line;
    } cases[] = {
        {"bom", baseline_log("\xEF\xBB\xBF"), kBaselineLine},
        {"long", baseline_log(std::string(), long_line), kBaselineLine},
        {"older", old_header,
         "\nBaseline: the vehicle's baseline recording, with no start time "
         "(unknown), profile obd2\n"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.folder);
        f.write(std::string(c.folder) + "/events.log", c.log);
        const std::string base = f.utf8(c.folder);
        const Reported text(f, f.utf8("rec"), ECUIO_HEALTH_TEXT, 0, 0,
                            base.c_str());
        ASSERT_EQ(text.status, ECUIO_OK) << text.detail;
        EXPECT_NE(text.bytes.find(c.line), std::string::npos) << text.bytes;
    }
    const Reported json(f, f.utf8("rec"), ECUIO_HEALTH_JSON, 0, 0,
                        f.utf8("older").c_str());
    const ecuio_json::Value v = parsed(json.bytes);
    const ecuio_json::Value* b = v.find("baseline");
    EXPECT_TRUE(b->find("read")->boolean);
    EXPECT_EQ(b->find("started")->type, ecuio_json::Type::null);
    EXPECT_EQ(b->find("length_s")->type, ecuio_json::Type::null);
    EXPECT_EQ(b->find("backend")->text, "unknown");
}

// ---- a cancel (ecuio_health_export_with) ----

/* Counts its asks and stops the report at the stop_at-th (0: never) */
struct Asks {
    unsigned long count = 0;
    unsigned long stop_at = 0;
};

int ask(void* user)
{
    auto* a = static_cast<Asks*>(user);
    ++a->count;
    return a->stop_at != 0 && a->count >= a->stop_at ? 1 : 0;
}

/* f's <folder> reported as JSON to <name>, through
 * ecuio_health_export_with with asks (NULL: a NULL cancel) */
ecuio_status_t report_with(const Folder& f, const std::string& folder,
                           const std::string& name, Asks* asks,
                           ecuio_health_report_t* r = nullptr)
{
    const std::string in = f.utf8(folder);
    const std::string out = f.utf8(name);
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.folder_utf8 = in.c_str();
    o.out_path_utf8 = out.c_str();
    o.latest_model_year = 2027;
    o.utc_us = kUtc;
    return ecuio_health_export_with(&o, asks != nullptr ? ask : nullptr, asks,
                                    r);
}

/* The replay fixture's frames under obd2, at <name> */
void fixture_recording(const Folder& f, const std::string& name)
{
    std::error_code ec;
    fs::create_directories(f.path / name, ec);
    fs::copy_file(fs::u8path(ECUIO_REPLAY_FIXTURES) /
                      "2026-10-25-engine-start" / "frames.csv",
                  f.path / name / "frames.csv", ec);
    ASSERT_FALSE(ec) << ec.message();
    f.write(name + "/events.log",
            "ecuio session recording, format 2 (ADR-0049)\n"
            "profile  obd2\n"
            "\n");
}

/* A cancel that never stops changes nothing: the same bytes as
 * ecuio_health_export's, and a NULL cancel is ecuio_health_export */
TEST(HealthCancelTest, ACancelThatNeverStopsWritesTheSameReport)
{
    Folder f;
    car_recording(f);
    const Reported plain(f, f.utf8("rec"), ECUIO_HEALTH_JSON);
    ASSERT_EQ(plain.status, ECUIO_OK) << plain.detail;
    Asks asks;
    ASSERT_EQ(report_with(f, "rec", "asked.json", &asks), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_GE(asks.count, 2u) << "the frames' pass and the values' open";
    EXPECT_EQ(read_file(f.path / "asked.json"), plain.bytes);
    ASSERT_EQ(report_with(f, "rec", "null.json", nullptr), ECUIO_OK);
    EXPECT_EQ(read_file(f.path / "null.json"), plain.bytes);
}

/* A stop at each ask in turn: CANCELED with its words and the report
 * zeroed, no ask after the one that stopped it, and no file at the name
 * or beside it */
TEST(HealthCancelTest, AStopAtEachAskInTurnWritesNothing)
{
    Folder f;
    car_recording(f);
    ASSERT_NO_FATAL_FAILURE(fixture_recording(f, "fixture"));
    for (const char* folder : {"rec", "fixture"}) {
        SCOPED_TRACE(folder);
        Asks all;
        ASSERT_EQ(
            report_with(f, folder, std::string(folder) + "-whole.json", &all),
            ECUIO_OK)
            << ecuio_last_error_detail();
        ASSERT_GE(all.count, 2u);
        for (unsigned long n = 1; n <= all.count; ++n) {
            Asks asks;
            asks.stop_at = n;
            ecuio_health_report_t r{};
            r.ecus = 7;
            ASSERT_EQ(report_with(f, folder, "stopped.json", &asks, &r),
                      ECUIO_ERR_CANCELED)
                << "at " << n;
            EXPECT_STREQ(ecuio_last_error_detail(), "the report was canceled");
            EXPECT_EQ(asks.count, n) << "asked again after it stopped";
            EXPECT_EQ(r.ecus | r.values | r.codes | r.failed_class, 0u)
                << "at " << n;
            ASSERT_FALSE(fs::exists(f.path / "stopped.json")) << "at " << n;
            ASSERT_FALSE(partial_left(f.path, "stopped.json")) << "at " << n;
        }
    }
}

/* The frames' pass is asked as its rows are read: the fixture's frames
 * (thousands of rows) add an ask each 1024 rows over the hand-written
 * recording's few (the report's own rule; the header promises no
 * interval) */
TEST(HealthCancelTest, TheFramesPassIsAskedAsItsRowsAreRead)
{
    Folder f;
    car_recording(f);
    ASSERT_NO_FATAL_FAILURE(fixture_recording(f, "fixture"));
    Asks few;
    ASSERT_EQ(report_with(f, "rec", "few.json", &few), ECUIO_OK);
    Asks many;
    ASSERT_EQ(report_with(f, "fixture", "many.json", &many), ECUIO_OK);
    const std::string frames = read_file(f.path / "fixture" / "frames.csv");
    const unsigned long rows = static_cast<unsigned long>(
        std::count(frames.begin(), frames.end(), '\n') - 1);
    ASSERT_GE(rows, 4096u);
    EXPECT_GE(many.count, rows / 1024) << rows << " rows";
    EXPECT_GT(many.count, few.count);
}

/* Where the report asks today (the report's own rule; the header names
 * the points, not a count): once as the frames' pass begins, before a
 * row is read, once each kCancelRows rows, and the reader's open at its
 * start and its end (a file under 4 MiB: none between). Over the car's
 * recording that open is signals.csv's, over the fixture's frames.csv's */
TEST(HealthCancelTest, EachPassIsAskedAtItsStartAndTheOpenAsItReads)
{
    Folder f;
    car_recording(f);
    ASSERT_NO_FATAL_FAILURE(fixture_recording(f, "fixture"));
    for (const char* folder : {"rec", "fixture"}) {
        SCOPED_TRACE(folder);
        const std::string frames = read_file(f.path / folder / "frames.csv");
        ASSERT_LT(frames.size(), 4u << 20);
        const unsigned long rows = static_cast<unsigned long>(
            std::count(frames.begin(), frames.end(), '\n') - 1);
        Asks asks;
        ASSERT_EQ(
            report_with(f, folder, std::string(folder) + "-asked.json", &asks),
            ECUIO_OK)
            << ecuio_last_error_detail();
        EXPECT_EQ(asks.count, 1 + rows / 1024 + 2) << rows << " rows";
    }

    // The first ask comes before frames.csv is read: a refused first row
    // is never reached
    f.write("rec/frames.csv",
            "host_us,timestamp_us,id,flags,data\nnot a row\n");
    Asks first;
    first.stop_at = 1;
    EXPECT_EQ(report_with(f, "rec", "first.json", &first), ECUIO_ERR_CANCELED);
    EXPECT_EQ(report_with(f, "rec", "first.json", nullptr),
              ECUIO_ERR_LOAD_FAILED);
}

/* A session's report reads no file: its cancel is never asked */
TEST(HealthCancelTest, ASessionsReportNeverAsks)
{
    Folder f;
    ecuio_session_t* s = nullptr;
    ASSERT_EQ(ecuio_session_create("mock://cancel", &s), ECUIO_OK);
    const std::string out = f.utf8("session.json");
    ecuio_health_options_t o{};
    o.format = ECUIO_HEALTH_JSON;
    o.session = s;
    o.out_path_utf8 = out.c_str();
    o.latest_model_year = 2027;
    o.utc_us = kUtc;
    Asks asks;
    asks.stop_at = 1;
    EXPECT_EQ(ecuio_health_export_with(&o, ask, &asks, nullptr), ECUIO_OK)
        << ecuio_last_error_detail();
    EXPECT_EQ(asks.count, 0u);
    EXPECT_TRUE(fs::exists(f.path / "session.json"));
    ecuio_session_destroy(s);
}

} // namespace
