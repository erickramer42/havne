// File/layer: app/recording_frameset.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#include "recording_frameset.h"

#include <cstring>
#include <utility>

namespace {

// setSignalFileText's hook, read and set on the GUI thread alone
RecordingFrameSet::SignalFileText& signalFileTextHook()
{
    static RecordingFrameSet::SignalFileText hook;
    return hook;
}

// Every count and every array's bytes alike, as tests' same_sets compares
// them: none of the four element structs has padding
bool sameSet(const ecuio_signal_set_t& a, const ecuio_signal_set_t& b)
{
    auto same = [](const void* x, const void* y, size_t n) {
        return n == 0 || std::memcmp(x, y, n) == 0;
    };
    return a.count == b.count && a.channel_count == b.channel_count &&
           a.enum_count == b.enum_count &&
           a.computed_count == b.computed_count &&
           same(a.defs, b.defs, sizeof *a.defs * a.count) &&
           same(a.channels, b.channels, sizeof *a.channels * a.channel_count) &&
           same(a.enums, b.enums, sizeof *a.enums * a.enum_count) &&
           same(a.computed, b.computed, sizeof *a.computed * a.computed_count);
}

} // namespace

void RecordingFrameSet::setSignalFileText(SignalFileText hook)
{
    signalFileTextHook() = std::move(hook);
}

QByteArray RecordingFrameSet::signalFileText(const ecuio_signal_set_t* set)
{
    return signalFileTextHook() ? signalFileTextHook()(set) : QByteArray();
}

RecordingFrameSet::RecordingFrameSet(ecuio_signal_file_t* own)
    : file(own, ecuio_signal_file_free)
{}

RecordingFrameSet::RecordingFrameSet(const ecuio_signal_set_t& s)
    : defs(s.defs, s.defs + (s.defs != nullptr ? s.count : 0)),
      channels(s.channels,
               s.channels + (s.channels != nullptr ? s.channel_count : 0)),
      enums(s.enums, s.enums + (s.enums != nullptr ? s.enum_count : 0)),
      computed(s.computed,
               s.computed + (s.computed != nullptr ? s.computed_count : 0))
{
    set = s;
    set.defs = defs.empty() ? nullptr : defs.data();
    set.channels = channels.empty() ? nullptr : channels.data();
    set.enums = enums.empty() ? nullptr : enums.data();
    set.computed = computed.empty() ? nullptr : computed.data();
}

void RecordingFrameSet::loadText()
{
    ecuio_signal_file_t* loaded = nullptr;
    if (fileText.isEmpty() ||
        ecuio_signal_file_load_with(
            fileText.constData(), static_cast<size_t>(fileText.size()),
            ECUIO_SIGNAL_FILE_LOAD_SET, &loaded) != ECUIO_OK) {
        return;
    }
    file.reset(loaded);
    if (!sameSet(*ecuio_signal_file_set(loaded), set)) {
        file.reset();
    }
}

const ecuio_signal_set_t* RecordingFrameSet::openSet() const
{
    return file ? ecuio_signal_file_set(file.get()) : &set;
}
