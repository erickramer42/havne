#include "test_cli_support.h"

#include "enhanced_codes_line.h"

namespace {

std::string line_of(const ecuio_module_codes_t& r,
                    const ecuio_channel_diagnostics_t* standard = nullptr)
{
    std::vector<char> buf(cli::kEnhancedCodesLineBytes);
    cli::Line l(buf.data(), buf.size());
    cli::enhanced_codes_line(l, r, standard);
    return std::string(l.data(), l.size());
}

ecuio_module_codes_t answered()
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE |
              ECUIO_MODULE_CODES_FORMAT;
    r.rx_id = 0x7E8;
    r.read_id = 1;
    r.generation = 2;
    r.mask = 0xAF;
    r.available = 0xFF;
    r.format = ECUIO_DTC_FORMAT_J2012;
    r.count_outcome = ECUIO_DIAG_ANSWERED;
    r.list_outcome = ECUIO_DIAG_ANSWERED;
    r.count = 2;
    r.reported = 2;
    r.held = 2;
    r.codes[0] = {0x012800, 0x20, {}};
    r.codes[1] = {0x030100, 0x08, {}};
    return r;
}

} // namespace

TEST(CliEnhancedCodesTest, AnAnsweredListCarriesEachCodesWords)
{
    const std::string line = line_of(answered());
    EXPECT_NE(line.find("\"type\":\"enhanced_codes\",\"rx\":\"0x7E8\","
                        "\"read\":1,\"generation\":2,\"mask\":\"0xAF\","),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("\"count\":{\"outcome\":\"answered\",\"format\":"
                        "\"SAE J2012\",\"available\":\"0xFF\",\"reported\":2}"),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("\"codes\":[\"P0128-00\",\"P0301-00\"]"),
              std::string::npos);
    EXPECT_NE(line.find("\"status\":[\"0x20\",\"0x8\"]"), std::string::npos);
    EXPECT_NE(line.find("\"state\":[\"Not now\",\"Not now\"]"),
              std::string::npos);
    EXPECT_NE(line.find("\"class\":[\"History\",\"Stored\"]"),
              std::string::npos);
    EXPECT_NE(line.find("\"lamp\":[false,false]"), std::string::npos);
    EXPECT_NE(line.find("\"emissions\":[[],[]]"), std::string::npos);
    EXPECT_EQ(line.back(), '}');
}

TEST(CliEnhancedCodesTest, ALampAndAnActiveCodeAreNamed)
{
    ecuio_module_codes_t r = answered();
    r.codes[0] = {0x9A011C, 0x89, {}};
    r.count = r.reported = r.held = 1;
    const std::string line = line_of(r);
    EXPECT_NE(line.find("\"codes\":[\"B1A01-1C\"]"), std::string::npos) << line;
    EXPECT_NE(line.find("\"state\":[\"Active\"]"), std::string::npos);
    EXPECT_NE(line.find("\"class\":[\"Stored\"]"), std::string::npos);
    EXPECT_NE(line.find("\"lamp\":[true]"), std::string::npos);
}

TEST(CliEnhancedCodesTest, AClassOfNoneIsNull)
{
    ecuio_module_codes_t r = answered();
    r.codes[0] = {0x056311, 0x50, {}};
    r.count = r.reported = r.held = 1;
    EXPECT_NE(line_of(r).find("\"class\":[null]"), std::string::npos);
}

TEST(CliEnhancedCodesTest, ACodeInTheStandardListIsMarkedNotHidden)
{
    ecuio_channel_diagnostics_t standard{};
    standard.lists[ECUIO_DIAG_STORED].codes[0] = 0x0301;
    standard.lists[ECUIO_DIAG_STORED].held = 1;
    standard.lists[ECUIO_DIAG_PENDING].codes[0] = 0x0301;
    standard.lists[ECUIO_DIAG_PENDING].held = 1;
    const std::string line = line_of(answered(), &standard);
    EXPECT_NE(line.find("\"emissions\":[[],[\"stored\",\"pending\"]]"),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("\"codes\":[\"P0128-00\",\"P0301-00\"]"),
              std::string::npos);
}

TEST(CliEnhancedCodesTest, ATruckFormatReadsAsHexWithNoTitleOrMark)
{
    ecuio_module_codes_t r = answered();
    r.format = ECUIO_DTC_FORMAT_J1939;
    ecuio_channel_diagnostics_t standard{};
    standard.lists[ECUIO_DIAG_STORED].codes[0] = 0x0301;
    standard.lists[ECUIO_DIAG_STORED].held = 1;
    const std::string line = line_of(r, &standard);
    EXPECT_NE(line.find("\"format\":\"SAE J1939\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"codes\":[\"0x012800\",\"0x030100\"]"),
              std::string::npos);
    EXPECT_NE(line.find("\"emissions\":[[],[]]"), std::string::npos);
    EXPECT_NE(line.find("\"titles\":[null,null]"), std::string::npos);
}

TEST(CliEnhancedCodesTest, ATitleSitsBesideItsCode)
{
    ecuio_module_codes_t r = answered();
    r.codes[0] = {0x030100, 0x08, {}};
    r.count = r.reported = r.held = 1;
    EXPECT_NE(line_of(r).find("\"titles\":[\"Cylinder 1 Misfire Detected\"]"),
              std::string::npos);
}

TEST(CliEnhancedCodesTest, ARefusalCarriesItsReplyAndNoCodes)
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE;
    r.rx_id = 0x7E9;
    r.read_id = 1;
    r.mask = 0xAF;
    r.count_outcome = ECUIO_DIAG_REFUSED;
    r.count_nrc = 0x12;
    r.list_outcome = ECUIO_DIAG_REFUSED;
    r.list_nrc = 0x7F;
    const std::string line = line_of(r);
    EXPECT_NE(line.find("\"count\":{\"outcome\":\"refused\",\"reply\":"
                        "\"7F 19 12\"}"),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("\"list\":{\"outcome\":\"refused\",\"reply\":"
                        "\"7F 19 7F\"}"),
              std::string::npos)
        << line;
    EXPECT_EQ(line.find("\"codes\""), std::string::npos);
}

TEST(CliEnhancedCodesTest, AModuleThatAnsweredNothingIsNoResponse)
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE;
    r.rx_id = 0x7E9;
    r.count_outcome = ECUIO_DIAG_UNANSWERED;
    r.list_outcome = ECUIO_DIAG_UNANSWERED;
    EXPECT_EQ(line_of(r), "{\"type\":\"enhanced_codes\",\"rx\":\"0x7E9\","
                          "\"outcome\":\"no_response\"}");
}

TEST(CliEnhancedCodesTest, AModuleThatCouldNotBeOpenedSaysSo)
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE |
              ECUIO_MODULE_CODES_NOT_OPENED;
    r.rx_id = 0x7EA;
    EXPECT_EQ(line_of(r), "{\"type\":\"enhanced_codes\",\"rx\":\"0x7EA\","
                          "\"outcome\":\"not_opened\"}");
}

TEST(CliEnhancedCodesTest, ACutListNamesWhatWasHeld)
{
    ecuio_module_codes_t r = answered();
    r.reported = 200;
    r.held = ECUIO_MODULE_CODES_MAX;
    for (uint32_t i = 0; i < ECUIO_MODULE_CODES_MAX; ++i) {
        r.codes[i] = {0x012800, 0x20, {}};
    }
    const std::string line = line_of(r);
    EXPECT_NE(line.find("\"list\":{\"outcome\":\"answered\",\"reported\":200,"
                        "\"held\":128,"),
              std::string::npos)
        << line;
    EXPECT_EQ(line.back(), '}');
}

TEST(CliEnhancedCodesTest, TheEndLineCountsModulesNotReached)
{
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::enhanced_codes_end_line(l, 3, 1, 1, 1);
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"enhanced_codes_end\",\"modules\":3,\"read\":1,"
              "\"no_response\":1,\"not_reached\":1}");
}
