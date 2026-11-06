// File/layer: fuzz/fuzz_signal_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_signal_file

#include <ecuio/core.h>
#include <ecuio/profile.h>
#include <ecuio/signals.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace {

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const char* bytes = reinterpret_cast<const char*>(data);
    ecuio_signal_file_t* a = nullptr;
    const ecuio_status_t sa = ecuio_signal_file_load(bytes, size, &a);
    const std::string detail_a = ecuio_last_error_detail();
    ecuio_signal_file_t* b = nullptr;
    const ecuio_status_t sb = ecuio_signal_file_load(bytes, size, &b);
    check(sa == sb);
    check(detail_a == ecuio_last_error_detail());

    if (sa != ECUIO_OK) {
        check(a == nullptr && b == nullptr);
        check(sa == ECUIO_ERR_INVALID_ARGUMENT || sa == ECUIO_ERR_OUT_OF_MEMORY);
        check(!detail_a.empty());
        check(detail_a.find('\n') == std::string::npos);
        check(detail_a.find('\r') == std::string::npos);
        return 0;
    }
    check(a != nullptr && b != nullptr);

    const ecuio_signal_set_t* set = ecuio_signal_file_set(a);
    ecuio_validation_t why{};
    check(ecuio_validate_signal_set(set, &why) == ECUIO_VALIDATE_OK);
    check(set->count == ecuio_signal_file_set(b)->count);
    check(strcmp(ecuio_signal_file_profile(a), ecuio_signal_file_profile(b)) == 0);

    /* The profile name is one the registry takes, and the set registers */
    const char* profile = ecuio_signal_file_profile(a);
    check(strncmp(profile, "user.", 5) == 0);
    check(strlen(profile) <= ECUIO_PROFILE_NAME_MAX);
    check(ecuio_profile_register(profile, set, nullptr) == ECUIO_OK);
    check(ecuio_profile_unregister(profile) == ECUIO_OK);

    /* Each def reads a payload as long as its field needs, through the
     * decoder its kind takes, without a fault */
    uint8_t payload[ECUIO_FRAME_DATA_MAX] = {};
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        for (uint32_t k = 0; k < d.mux_length; ++k) {
            payload[d.mux_offset + k] =
                static_cast<uint8_t>(d.mux_key >> (8u * (d.mux_length - 1u - k)));
        }
        double v = 0.0;
        char text[ECUIO_DECODE_TEXT_MAX];
        if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
            (void)ecuio_decode_text(&d, payload, ECUIO_FRAME_DATA_MAX, text,
                                    sizeof text);
        } else if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0) {
            check(ecuio_decode_signal(&d, payload, ECUIO_FRAME_DATA_MAX, &v) ==
                  ECUIO_DECODE_OK);
            if (d.kind == ECUIO_VALUE_ENUM) {
                (void)ecuio_enum_label(set, &d, static_cast<uint32_t>(v));
            }
        }
    }

    ecuio_signal_file_free(a);
    ecuio_signal_file_free(b);
    return 0;
}
