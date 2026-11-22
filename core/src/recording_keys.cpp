// File/layer: core/src/recording_keys.cpp | C++17, Qt-free, every platform
// Testing: UNIT: RecorderTest, RecordingTest (test_core)
//
// recording_keys.h's calls that aren't inline.

#include "recording_keys.h"

namespace ecuio_recording_keys {

ecuio_status_t set_keys(const ecuio_signal_set_t* set,
                        const ecuio_signal_file_t* file,
                        std::vector<ecuio_signal_key_t>* out) noexcept
{
    out->clear();
    if (set == nullptr || set->defs == nullptr || set->count == 0) {
        return ECUIO_OK;
    }
    try {
        out->resize(set->count);
    } catch (...) {
        return ECUIO_ERR_OUT_OF_MEMORY;
    }
    uint32_t count = 0;
    const ecuio_status_t s =
        ecuio_signal_set_keys(set, file, out->data(), set->count, &count);
    if (s != ECUIO_OK) {
        out->clear();
    }
    return s;
}

} // namespace ecuio_recording_keys

extern "C" {

ecuio_status_t ecuio_recording_open_frames(const char* folder_utf8,
                                           const ecuio_signal_set_t* signal_set,
                                           ecuio_open_progress_t progress,
                                           void* user_data,
                                           ecuio_recording_t** out)
{
    return ecuio_recording_open_frames_with(folder_utf8, signal_set, nullptr,
                                            progress, user_data, out);
}

} // extern "C"
