#include "dump_lines.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

ecuio_operation_state_t state()
{
    ecuio_operation_state_t s{};
    s.struct_size = sizeof s;
    return s;
}

std::string text(void (*fill)(cli::Line&, const ecuio_operation_state_t&),
                 const ecuio_operation_state_t& s)
{
    char buf[cli::kDumpLineBytes];
    cli::Line l(buf, sizeof buf);
    fill(l, s);
    return std::string(l.data(), l.size());
}

std::string dump_text(const ecuio_operation_state_t& s, const std::string& path,
                      int verified)
{
    std::vector<char> buf(cli::kDumpLineBytes +
                          cli::Line::string_bytes(path.c_str()));
    cli::Line l(buf.data(), buf.size());
    cli::dump_line(l, s, path, verified);
    EXPECT_FALSE(l.full());
    return std::string(l.data(), l.size());
}

} // namespace

TEST(CliDumpLineTest, EveryPhaseAndEndHasItsWord)
{
    const std::vector<std::string> phases = {
        "none",      "checking", "preconditions", "unlocking", "reading",
        "comparing", "saving",   "leaving",       "done"};
    for (uint32_t i = 0; i < phases.size(); ++i) {
        EXPECT_EQ(cli::dump_phase_word(i), phases[i]);
    }
    EXPECT_STREQ(cli::dump_phase_word(ECUIO_OPERATION_PHASE_DONE + 1),
                 "unknown");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_SAVED), "saved");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_CANCELED), "canceled");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_FILE), "file");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_ENGINE_RUNNING),
                 "engine_running");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_VOLTAGE_HIGH),
                 "voltage_high");
    EXPECT_STREQ(cli::dump_end_word(ECUIO_OPERATION_END_VOLTAGE_HIGH + 1),
                 "unknown");
}

TEST(CliDumpLineTest, EachWarningSaysItsReading)
{
    ecuio_operation_state_t s = state();
    s.checks = ECUIO_OPERATION_CHECK_VOLTS;
    s.volts_mv = 16420;
    s.volts_min_mv = 10950;
    EXPECT_EQ(cli::dump_warning_words(ECUIO_OPERATION_CHECK_VOLTS_HIGH, s),
              "the supply is at 16.4 V: check the charger");
    EXPECT_EQ(cli::dump_warning_words(ECUIO_OPERATION_CHECK_VOLTS_SAG, s),
              "the battery is at 10.9 V: the car may not start");
    s.volts_mv = 11600;
    EXPECT_EQ(cli::dump_warning_words(ECUIO_OPERATION_CHECK_VOLTS_LOW, s),
              "the battery is at 11.6 V: keep a charger on");
    s.checks = 0;
    EXPECT_EQ(cli::dump_warning_words(ECUIO_OPERATION_CHECK_VOLTS_LOW, s),
              "the battery is low: keep a charger on");
    for (const cli::DumpWarning& w : cli::kDumpWarnings) {
        EXPECT_FALSE(cli::dump_warning_words(w.bit, s).empty()) << w.word;
    }
}

TEST(CliDumpLineTest, ASameSecondClashIsSaidInWords)
{
    ecuio_operation_state_t s = state();
    s.end = ECUIO_OPERATION_END_FILE;
    std::strcpy(s.words, "the copy can't be put in place: it exists already");
    ASSERT_NE(cli::dump_end_note(s), nullptr);
    EXPECT_NE(std::strstr(cli::dump_end_note(s), "same second"), nullptr);
    std::strcpy(s.words, "the folder can't be written");
    EXPECT_EQ(cli::dump_end_note(s), nullptr);
}

TEST(CliDumpLineTest, ProgressAndPhaseLines)
{
    ecuio_operation_state_t s = state();
    s.phase = ECUIO_OPERATION_PHASE_READING;
    s.step = 1;
    s.pass = 2;
    s.bytes_done = 3000;
    s.bytes_total = 4000;
    s.waiting_ms = 1500;
    EXPECT_EQ(text(cli::progress_line, s),
              R"({"type":"progress","bytes_done":3000,"bytes_total":4000,)"
              R"("percent":75,"waiting_ms":1500})");
    char buf[cli::kDumpLineBytes];
    cli::Line l(buf, sizeof buf);
    cli::phase_line(l, s, "code");
    EXPECT_EQ(std::string(l.data(), l.size()),
              R"({"type":"phase","phase":"reading","step":1,"name":"code",)"
              R"("pass":2})");
}

TEST(CliDumpLineTest, ASavedCopyNamesItsFileAndChecksum)
{
    ecuio_operation_state_t s = state();
    s.end = ECUIO_OPERATION_END_SAVED;
    s.bytes_total = 8192;
    const std::string hash(64, 'a');
    const std::string name = "20261127-7E0-" + hash + ".havnedump";
    std::strcpy(s.name, name.c_str());
    const std::string line = dump_text(s, "out/" + name, 1);
    EXPECT_NE(line.find(R"("end":"saved","words":null)"), std::string::npos);
    EXPECT_NE(line.find(R"("sha256":")" + hash + "\""), std::string::npos);
    EXPECT_NE(line.find(R"("bytes":4096,"verified":true,"nrc":null,)"
                        R"("differ_offset":null,"volts_mv":null)"),
              std::string::npos);
    EXPECT_EQ(cli::dump_name_hash("short.havnedump"), "");
    EXPECT_EQ(cli::dump_name_hash((name + "x").c_str()), "");
}

TEST(CliDumpLineTest, AnEndWithoutACopyNamesNoFile)
{
    ecuio_operation_state_t s = state();
    s.end = ECUIO_OPERATION_END_DIFFER;
    s.differ_offset = 77;
    s.nrc = 0x31;
    s.checks = ECUIO_OPERATION_CHECK_VOLTS | ECUIO_OPERATION_CHECK_RPM;
    s.volts_mv = 12500;
    s.volts_min_mv = 12400;
    std::strcpy(s.name, "ignored.havnedump");
    const std::string line = dump_text(s, "", -1);
    EXPECT_NE(line.find(R"("name":null,"path":null,"sha256":null)"),
              std::string::npos);
    EXPECT_NE(line.find(R"("verified":null,"nrc":"31","differ_offset":77,)"
                        R"("volts_mv":12500,"volts_min_mv":12400,"rpm":0,)"
                        R"("kmh":null})"),
              std::string::npos);
}

TEST(CliDumpLineTest, ALongPathFitsItsLine)
{
    ecuio_operation_state_t s = state();
    s.end = ECUIO_OPERATION_END_SAVED;
    std::strcpy(s.name, "x.havnedump");
    const std::string path(4000, '\x01');
    const std::string line = dump_text(s, path, 0);
    EXPECT_EQ(line.back(), '}');
}
