// File/layer: fuzz/fuzz_vin.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary text as a VIN and as a manufacturer code
//          (ADR-0043): the status is one of two, a refusal leaves the info
//          zeroed, a decode's fields stay in their ranges, a make is never
//          NULL, two calls agree

#include <ecuio/core.h>
#include <ecuio/vin.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* A VIN is 17 characters; past a few dozen nothing new is reached */
    if (size < 2 || size > 64) {
        return 0;
    }

    /* The first two bytes are the latest model year (0 none); the rest
     * is the text, a NUL becoming a letter so the C string runs whole */
    const uint32_t latest = static_cast<uint32_t>(data[0]) << 8 | data[1];
    std::string text;
    for (size_t i = 2; i < size; ++i) {
        text.push_back(data[i] == 0 ? 'x' : static_cast<char>(data[i]));
    }

    ecuio_vin_info_t a;
    ecuio_vin_info_t b;
    memset(&a, 0xA5, sizeof a);
    memset(&b, 0x5A, sizeof b);
    const ecuio_status_t ra = ecuio_vin_decode(text.c_str(), latest, &a);
    const ecuio_status_t rb = ecuio_vin_decode(text.c_str(), latest, &b);
    if (ra != rb || memcmp(&a, &b, sizeof a) != 0) {
        abort();
    }
    if (ra == ECUIO_ERR_INVALID_ARGUMENT) {
        const ecuio_vin_info_t zero{};
        if (memcmp(&a, &zero, sizeof a) != 0) {
            abort();
        }
    } else if (ra == ECUIO_OK) {
        if (text.size() != ECUIO_VIN_LEN) {
            abort();
        }
        if (a.flags & ~(ECUIO_VIN_CHECK_MATCHES | ECUIO_VIN_YEAR_EARLIER)) {
            abort();
        }
        if (a.model_year != 0 && (a.model_year < 1980 || a.model_year > 2039)) {
            abort();
        }
        if (a.model_year == 0 && (a.flags & ECUIO_VIN_YEAR_EARLIER)) {
            abort();
        }
        if (latest != 0 && a.model_year > 2009 && a.model_year > latest) {
            abort();
        }
        const char c = a.check_digit;
        if (!((c >= '0' && c <= '9') || c == 'X')) {
            abort();
        }
        if (((a.flags & ECUIO_VIN_CHECK_MATCHES) != 0) != (text[8] == c)) {
            abort();
        }
        if (a.wmi_length != 3 && a.wmi_length != 6) {
            abort();
        }
        if (strnlen(a.wmi, sizeof a.wmi) != a.wmi_length) {
            abort();
        }
        for (uint32_t r : a.reserved) {
            if (r != 0) {
                abort();
            }
        }
        if (ecuio_vin_make(a.wmi) == nullptr) {
            abort();
        }
    } else {
        abort();
    }
    /* The text as a code: never NULL, the same twice */
    const char* m1 = ecuio_vin_make(text.c_str());
    const char* m2 = ecuio_vin_make(text.c_str());
    if (m1 == nullptr || m1 != m2) {
        abort();
    }
    return 0;
}
