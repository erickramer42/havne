// File/layer: core/src/dtc_titles.cpp | C++17, Qt-free
// Testing: UNIT: DtcTitleTest (test_obd2): titles found, the ranges left
//          out, the parse against ecuio_dtc_format over every code; the
//          table's order is a static_assert here
//
// ecuio_dtc_parse, ecuio_dtc_title, ecuio_dtc_title_source and
// ecuio_dtc_title_notice (core.h,
// ADR-0044): a code's title looked up in dtc_titles_table.h, which
// scripts/dtc_titles.py writes from Wal33D's dtc-database.

#include "ecuio/core.h"

#include "diagnostics.h"
#include "dtc_titles_table.h"

#include <cstddef>

namespace {

using ecuio_dtc::kTitles;

constexpr std::size_t kCount = sizeof kTitles / sizeof kTitles[0];

constexpr bool table_ok()
{
    for (std::size_t i = 0; i < kCount; ++i) {
        if (kTitles[i].title == nullptr || kTitles[i].title[0] == '\0') {
            return false;
        }
        if (i > 0 && kTitles[i - 1].code >= kTitles[i].code) {
            return false;
        }
    }
    return true;
}
static_assert(table_ok(), "dtc_titles_table.h: codes sorted and unique, "
                          "every title text");

constexpr int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_dtc_parse(const char* text, uint16_t* out)
{
    if (text == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    // The letter is the code's top two bits (ecuio_dtc_format's order)
    static const char kLetters[] = "PCBU";
    int letter = -1;
    for (int i = 0; i < 4; ++i) {
        if (text[0] == kLetters[i]) {
            letter = i;
        }
    }
    if (letter < 0) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    unsigned value = static_cast<unsigned>(letter) << 14;
    for (int i = 1; i <= 4; ++i) {
        const int v = hex_value(text[i]);
        // The first digit has two bits: 0-3
        if (v < 0 || (i == 1 && v > 3)) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        value |= static_cast<unsigned>(v) << (4 * (4 - i));
    }
    if (text[5] != '\0') {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *out = static_cast<uint16_t>(value);
    return ECUIO_OK;
}

const char* ecuio_dtc_title(uint16_t code)
{
    std::size_t lo = 0;
    std::size_t hi = kCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kTitles[mid].code == code) {
            return kTitles[mid].title;
        }
        if (kTitles[mid].code < code) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return "";
}

const char* ecuio_dtc_title_source(void)
{
    return ecuio_dtc::kTitlesSource;
}

const char* ecuio_dtc_title_notice(void)
{
    return ecuio_dtc::kTitlesNotice;
}

ecuio_status_t ecuio_dtc_format_uds(uint32_t code, uint8_t format, char* out,
                                    uint32_t out_cap)
{
    if (out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out_cap < ECUIO_DTC_UDS_TEXT_LEN) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    static const char kHex[] = "0123456789ABCDEF";
    if (ecuio_dtc_uds_is_j2012(format)) {
        ecuio_dtc_text(static_cast<uint16_t>((code >> 8) & 0xFFFFu), out);
        out[5] = '-';
        out[6] = kHex[(code >> 4) & 0xFu];
        out[7] = kHex[code & 0xFu];
        out[8] = '\0';
        return ECUIO_OK;
    }
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 6; ++i) {
        out[2 + i] = kHex[(code >> (4 * (5 - i))) & 0xFu];
    }
    out[8] = '\0';
    return ECUIO_OK;
}

const char* ecuio_dtc_title_uds(uint32_t code, uint8_t format)
{
    return ecuio_dtc_uds_is_j2012(format)
               ? ecuio_dtc_title(static_cast<uint16_t>((code >> 8) & 0xFFFFu))
               : "";
}

const char* ecuio_dtc_status_state(uint8_t status)
{
    return (status & ECUIO_DTC_STATUS_TEST_FAILED) != 0 ? "Active" : "Not now";
}

const char* ecuio_dtc_status_class(uint8_t status)
{
    if ((status & ECUIO_DTC_STATUS_CONFIRMED) != 0) {
        return "Stored";
    }
    if ((status & ECUIO_DTC_STATUS_PENDING) != 0) {
        return "Pending";
    }
    if ((status & ECUIO_DTC_STATUS_FAILED_SINCE_CLEAR) != 0) {
        return "History";
    }
    return "";
}

int ecuio_dtc_status_lamp(uint8_t status)
{
    return (status & ECUIO_DTC_STATUS_LAMP) != 0 ? 1 : 0;
}

uint32_t
ecuio_module_code_emissions_lists(const ecuio_channel_diagnostics_t* report,
                                  uint32_t code, uint8_t format)
{
    if (report == nullptr || !ecuio_dtc_uds_is_j2012(format)) {
        return 0;
    }
    const uint16_t j1979 = static_cast<uint16_t>((code >> 8) & 0xFFFFu);
    uint32_t lists = 0;
    for (uint32_t k = 0; k < ECUIO_DIAG_LISTS; ++k) {
        const ecuio_dtc_list_t& l = report->lists[k];
        const uint32_t held =
            l.held < ECUIO_DIAG_MAX_DTCS ? l.held : ECUIO_DIAG_MAX_DTCS;
        for (uint32_t i = 0; i < held; ++i) {
            if (l.codes[i] == j1979) {
                lists |= 1u << k;
                break;
            }
        }
    }
    return lists;
}

} // extern "C"
