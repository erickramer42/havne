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

} // extern "C"
