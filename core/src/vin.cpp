// File/layer: core/src/vin.cpp | C++17, Qt-free
// Testing: UNIT: VinTest (test_core)
//
// ecuio_vin_decode, ecuio_vin_make and ecuio_vin_source (vin.h,
// ADR-0043): the VIN's model year and check digit by 49 CFR 565.15, its
// manufacturer code looked up in vin_wmi_table.h, which
// scripts/vpic_wmi.py writes from NHTSA's vPIC database.

#include "ecuio/vin.h"

#include "vin_wmi_table.h"

#include <cstddef>
#include <cstring>

namespace {

using ecuio_vin::kMakes;
using ecuio_vin::kWmis;
using ecuio_vin::Wmi;

constexpr bool vin_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
}

/* 565.15(c)(1), Table III: a digit is its value, a letter its own */
constexpr int vin_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    constexpr const char* kLetters = "ABCDEFGHJKLMNPRSTUVWXYZ";
    constexpr int kValues[] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4,
                               5, 7, 9, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; kLetters[i] != '\0'; ++i) {
        if (kLetters[i] == c) {
            return kValues[i];
        }
    }
    return 0;
}

/* Table IV, position 9's weight 0 */
constexpr int kWeights[ECUIO_VIN_LEN] = {8, 7, 6, 5, 4, 3, 2, 10, 0,
                                         9, 8, 7, 6, 5, 4, 3, 2};

/* Table VII's codes from 1980, a cycle of 30: A is 1980 and 2010, Y
 * 2000 and 2030, 1 2001 and 2031, 9 2009 and 2039 */
constexpr const char* kYearCodes = "ABCDEFGHJKLMNPRSTVWXY123456789";

constexpr int code_cmp(const char* a, const char* b)
{
    for (int i = 0;; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
}

constexpr bool table_ok()
{
    constexpr std::size_t n = sizeof kWmis / sizeof kWmis[0];
    for (std::size_t i = 0; i < n; ++i) {
        const Wmi& w = kWmis[i];
        int len = 0;
        while (w.code[len] != '\0') {
            if (!vin_char(w.code[len])) {
                return false;
            }
            ++len;
        }
        // Three characters, or six exactly when the third is a 9
        if (len != (len >= 3 && w.code[2] == '9' ? 6 : 3)) {
            return false;
        }
        if (w.make >= sizeof kMakes / sizeof kMakes[0]) {
            return false;
        }
        if (i > 0 && code_cmp(kWmis[i - 1].code, w.code) >= 0) {
            return false;
        }
    }
    return true;
}
static_assert(table_ok(), "vin_wmi_table.h: codes sorted, unique, the VIN "
                          "alphabet's, 3 or 6 long, each make in range");

} // namespace

extern "C" {

ecuio_status_t ecuio_vin_decode(const char* vin, uint32_t latest_model_year,
                                ecuio_vin_info_t* out)
{
    if (vin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *out = ecuio_vin_info_t{};
    int sum = 0;
    for (int i = 0; i < ECUIO_VIN_LEN; ++i) {
        if (!vin_char(vin[i])) {
            return ECUIO_ERR_INVALID_ARGUMENT;
        }
        sum += vin_value(vin[i]) * kWeights[i];
    }
    if (vin[ECUIO_VIN_LEN] != '\0') {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }

    const int remainder = sum % 11;
    out->check_digit =
        remainder == 10 ? 'X' : static_cast<char>('0' + remainder);
    if (vin[8] == out->check_digit) {
        out->flags |= ECUIO_VIN_CHECK_MATCHES;
    }

    const char* code = std::strchr(kYearCodes, vin[9]);
    if (code != nullptr) {
        // Table VII's note: position 7 a letter is 2010-2039
        const bool later = vin[6] >= 'A' && vin[6] <= 'Z';
        int year =
            1980 + static_cast<int>(code - kYearCodes) + (later ? 30 : 0);
        if (later && latest_model_year != 0 &&
            static_cast<uint32_t>(year) > latest_model_year) {
            year -= 30;
            out->flags |= ECUIO_VIN_YEAR_EARLIER;
        }
        out->model_year = static_cast<uint16_t>(year);
    }

    // 565.15(a): a 9 in position 3 is a six-character code, positions
    // 1-3 and 12-14
    std::memcpy(out->wmi, vin, 3);
    out->wmi_length = 3;
    if (vin[2] == '9') {
        std::memcpy(out->wmi + 3, vin + 11, 3);
        out->wmi_length = 6;
    }
    return ECUIO_OK;
}

const char* ecuio_vin_make(const char* wmi)
{
    if (wmi == nullptr) {
        return "";
    }
    std::size_t lo = 0;
    std::size_t hi = sizeof kWmis / sizeof kWmis[0];
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int c = code_cmp(kWmis[mid].code, wmi);
        if (c == 0) {
            return kMakes[kWmis[mid].make];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return "";
}

const char* ecuio_vin_source(void)
{
    return ecuio_vin::kVpicRelease;
}

} // extern "C"
