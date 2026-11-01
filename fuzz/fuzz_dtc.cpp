// File/layer: fuzz/fuzz_dtc.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: arbitrary text as a trouble code (ADR-0044): the status
//          is one of two, a refusal leaves the code untouched, a parse
//          formats back to the same text, and its title is never NULL

#include <ecuio/core.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* A code is five characters; a few dozen reach every refusal */
    if (size > 32) {
        return 0;
    }
    /* A NUL becomes a letter so the C string runs whole */
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text.push_back(data[i] == 0 ? 'x' : static_cast<char>(data[i]));
    }

    uint16_t code = 0xA5A5;
    const ecuio_status_t rc = ecuio_dtc_parse(text.c_str(), &code);
    if (rc == ECUIO_ERR_INVALID_ARGUMENT) {
        if (code != 0xA5A5) {
            abort();
        }
    } else if (rc == ECUIO_OK) {
        char back[8] = {};
        if (ecuio_dtc_format(code, back, sizeof back) != ECUIO_OK ||
            text != back) {
            abort();
        }
        if (ecuio_dtc_title(code) == nullptr) {
            abort();
        }
    } else {
        abort();
    }
    return 0;
}
