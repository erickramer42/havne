#include "diagnostics.h"

#include <ecuio/core.h>

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

std::string format_uds(uint32_t code, uint8_t format)
{
    char out[ECUIO_DTC_UDS_TEXT_LEN];
    EXPECT_EQ(ecuio_dtc_format_uds(code, format, out, sizeof out), ECUIO_OK);
    return out;
}

void put_record(std::vector<uint8_t>& reply, uint32_t code, uint8_t status)
{
    reply.push_back(static_cast<uint8_t>(code >> 16));
    reply.push_back(static_cast<uint8_t>(code >> 8));
    reply.push_back(static_cast<uint8_t>(code));
    reply.push_back(status);
}

} // namespace

TEST(DtcUdsTest, AJ2012CodeReadsAsItsLetterDigitsAndFailureType)
{
    for (uint8_t format : std::initializer_list<uint8_t>{
             ECUIO_DTC_FORMAT_J2012, ECUIO_DTC_FORMAT_ISO14229,
             ECUIO_DTC_FORMAT_J2012_WWH}) {
        EXPECT_EQ(format_uds(0x012800, format), "P0128-00");
        EXPECT_EQ(format_uds(0xD12345, format), "U1123-45");
        EXPECT_EQ(format_uds(0x9A011C, format), "B1A01-1C");
    }
    char out[ECUIO_DTC_UDS_TEXT_LEN];
    std::memset(out, 'x', sizeof out);
    EXPECT_EQ(
        ecuio_dtc_format_uds(0x012800, 0, out, ECUIO_DTC_UDS_TEXT_LEN - 1),
        ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out[0], 'x');
    EXPECT_EQ(ecuio_dtc_format_uds(0x012800, 0, nullptr, sizeof out),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(DtcUdsTest, ATruckFormatReadsAsHex)
{
    EXPECT_EQ(format_uds(0x012800, ECUIO_DTC_FORMAT_J1939), "0x012800");
    EXPECT_EQ(format_uds(0x012800, ECUIO_DTC_FORMAT_ISO11992), "0x012800");
    EXPECT_EQ(format_uds(0xFEDCBA, ECUIO_DTC_FORMAT_J1939), "0xFEDCBA");
}

TEST(DtcUdsTest, TheTitleIsTheGenericCodesForJ2012Formats)
{
    ASSERT_NE(ecuio_dtc_title(0x0128)[0], '\0');
    EXPECT_STREQ(ecuio_dtc_title_uds(0x012800, ECUIO_DTC_FORMAT_J2012),
                 ecuio_dtc_title(0x0128));
    EXPECT_STREQ(ecuio_dtc_title_uds(0x012845, ECUIO_DTC_FORMAT_J2012_WWH),
                 ecuio_dtc_title(0x0128));
    EXPECT_STREQ(ecuio_dtc_title_uds(0x012800, ECUIO_DTC_FORMAT_J1939), "");
}

TEST(DtcUdsTest, TheStatusStateIsActiveOnBitZero)
{
    for (uint8_t s : std::initializer_list<uint8_t>{0x01, 0x09, 0x21, 0x89}) {
        EXPECT_STREQ(ecuio_dtc_status_state(s), "Active") << int{s};
    }
    for (uint8_t s :
         std::initializer_list<uint8_t>{0x08, 0x28, 0x02, 0x50, 0x00}) {
        EXPECT_STREQ(ecuio_dtc_status_state(s), "Not now") << int{s};
    }
}

TEST(DtcUdsTest, TheStatusClassFollowsTheTable)
{
    for (uint8_t s :
         std::initializer_list<uint8_t>{0x09, 0x08, 0x28, 0x68, 0x2C}) {
        EXPECT_STREQ(ecuio_dtc_status_class(s), "Stored") << int{s};
    }
    for (uint8_t s : std::initializer_list<uint8_t>{0x05, 0x24}) {
        EXPECT_STREQ(ecuio_dtc_status_class(s), "Pending") << int{s};
    }
    for (uint8_t s : std::initializer_list<uint8_t>{0x20, 0x60, 0x21}) {
        EXPECT_STREQ(ecuio_dtc_status_class(s), "History") << int{s};
    }
    for (uint8_t s : std::initializer_list<uint8_t>{0x01, 0x02, 0x50, 0x80}) {
        EXPECT_STREQ(ecuio_dtc_status_class(s), "") << int{s};
    }
}

TEST(DtcUdsTest, TheLampIsBitSevenAlone)
{
    EXPECT_EQ(ecuio_dtc_status_lamp(0x80), 1);
    EXPECT_EQ(ecuio_dtc_status_lamp(0x89), 1);
    EXPECT_EQ(ecuio_dtc_status_lamp(0x7F), 0);
}

TEST(DtcUdsTest, AnEmissionsListMarksTheSameCodeByItsFirstTwoBytes)
{
    ecuio_channel_diagnostics_t report{};
    ecuio_dtc_list_t& stored = report.lists[ECUIO_DIAG_STORED];
    stored.codes[0] = 0x0301;
    stored.codes[1] = 0x0420;
    stored.held = stored.reported = 2;
    ecuio_dtc_list_t& permanent = report.lists[ECUIO_DIAG_PERMANENT];
    permanent.codes[0] = 0x0420;
    permanent.held = permanent.reported = 1;

    EXPECT_EQ(ecuio_module_code_emissions_lists(&report, 0x030100,
                                                ECUIO_DTC_FORMAT_J2012),
              1u << ECUIO_DIAG_STORED);
    EXPECT_EQ(ecuio_module_code_emissions_lists(&report, 0x03017A,
                                                ECUIO_DTC_FORMAT_ISO14229),
              1u << ECUIO_DIAG_STORED);
    EXPECT_EQ(ecuio_module_code_emissions_lists(&report, 0x042000,
                                                ECUIO_DTC_FORMAT_J2012_WWH),
              (1u << ECUIO_DIAG_STORED) | (1u << ECUIO_DIAG_PERMANENT));
    EXPECT_EQ(ecuio_module_code_emissions_lists(&report, 0x012800,
                                                ECUIO_DTC_FORMAT_J2012),
              0u);
    EXPECT_EQ(ecuio_module_code_emissions_lists(&report, 0x030100,
                                                ECUIO_DTC_FORMAT_J1939),
              0u);
    EXPECT_EQ(ecuio_module_code_emissions_lists(nullptr, 0x030100, 0), 0u);
}

TEST(DtcUdsTest, AListHoldsWholeRecordsAndLeavesOutTheFill)
{
    std::vector<uint8_t> reply{0x59, 0x02, 0xFF};
    put_record(reply, 0x012800, 0x20);
    put_record(reply, 0x000000, 0x00);
    put_record(reply, 0xD12345, 0x24);
    put_record(reply, 0x9A011C, 0x89);
    reply.push_back(0x03);
    reply.push_back(0x01);
    ecuio_module_codes_t r{};
    ecuio_diag_take_uds_list(reply.data(), static_cast<uint32_t>(reply.size()),
                             &r);
    EXPECT_EQ(r.available, 0xFF);
    EXPECT_EQ(r.reported, 3);
    ASSERT_EQ(r.held, 3);
    EXPECT_EQ(r.codes[0].code, 0x012800u);
    EXPECT_EQ(r.codes[0].status, 0x20);
    EXPECT_EQ(r.codes[1].code, 0xD12345u);
    EXPECT_EQ(r.codes[2].code, 0x9A011Cu);
    EXPECT_EQ(r.codes[2].status, 0x89);
}

TEST(DtcUdsTest, AListPastTheCapCountsAllAndHoldsTheCap)
{
    std::vector<uint8_t> reply{0x59, 0x02, 0xFF};
    for (uint32_t i = 0; i < 200; ++i) {
        put_record(reply, 0x010000 + i, 0x08);
    }
    ecuio_module_codes_t r{};
    ecuio_diag_take_uds_list(reply.data(), static_cast<uint32_t>(reply.size()),
                             &r);
    EXPECT_EQ(r.reported, 200);
    EXPECT_EQ(r.held, ECUIO_MODULE_CODES_MAX);
    EXPECT_EQ(r.codes[ECUIO_MODULE_CODES_MAX - 1].code,
              0x010000u + ECUIO_MODULE_CODES_MAX - 1);
}

TEST(DtcUdsTest, ACountGivesTheFormatOnlyWhole)
{
    const uint8_t whole[] = {0x59, 0x01, 0xFF, 0x00, 0x00, 0x03};
    ecuio_module_codes_t r{};
    EXPECT_TRUE(ecuio_diag_take_uds_count(whole, sizeof whole, &r));
    EXPECT_NE(r.flags & ECUIO_MODULE_CODES_FORMAT, 0u);
    EXPECT_EQ(r.format, ECUIO_DTC_FORMAT_J2012);
    EXPECT_EQ(r.count, 3);
    EXPECT_EQ(r.available, 0xFF);

    const uint8_t wide[] = {0x59, 0x01, 0x09, 0x02, 0x01, 0x02};
    ecuio_module_codes_t w{};
    EXPECT_TRUE(ecuio_diag_take_uds_count(wide, sizeof wide, &w));
    EXPECT_EQ(w.format, ECUIO_DTC_FORMAT_J1939);
    EXPECT_EQ(w.count, 0x0102);

    ecuio_module_codes_t s{};
    EXPECT_FALSE(ecuio_diag_take_uds_count(whole, 5, &s));
    EXPECT_EQ(s.flags, 0u);
    EXPECT_EQ(s.available, 0);
    EXPECT_EQ(s.count, 0);
}
