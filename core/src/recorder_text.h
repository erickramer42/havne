// File/layer: core/src/recorder_text.h | internal, C++17, Qt-free; the
//             recorder's text helpers and its folder's naming (ADR-0049)
// Testing: UNIT: RecorderTest, ConditionRecordingTest,
//          ArmedRecorderTest (test_core);
//          BRIDGE: BridgeTest (test_bridge);
//          HARNESS: Elm327SessionTest (test_elm327)
//
// Defined in recorder_text.cpp, but for focus_parts_with, a template the
// armed writer calls with its own names.
#ifndef ECUIO_RECORDER_TEXT_H
#define ECUIO_RECORDER_TEXT_H

#include "recorder_impl.h"

#include "ecuio/signals.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace ecuio_rec {

std::tm local_tm(std::time_t t);

/* The most of a focus line's names: a large pick of long names takes
 * several lines, each far inside a row's bound */
constexpr size_t kFocusPartBytes = 4096;

/* The picks as signals.csv names them, in pick order, in parts of at
 * most kFocusPartBytes (one never split); named counts them.
 * name_of(id, &name) finds what a pick is written as; one it lacks
 * names nothing. */
template <typename NameOf>
std::vector<std::string> focus_parts_with(const std::vector<uint16_t>& ids,
                                          NameOf name_of, size_t* named)
{
    std::vector<std::string> parts;
    *named = 0;
    for (uint16_t id : ids) {
        std::string name;
        if (!name_of(id, &name)) {
            continue;
        }
        if (parts.empty() ||
            parts.back().size() + 1 + name.size() > kFocusPartBytes) {
            parts.push_back(name);
        } else {
            parts.back() += ',';
            parts.back() += name;
        }
        ++*named;
    }
    return parts;
}

std::vector<std::string> focus_parts(const std::vector<def_entry>& defs,
                                     const std::vector<uint16_t>& ids,
                                     size_t* named);
std::string focus_header(const std::vector<std::string>& parts, size_t named,
                         const focus_named& f);
std::string one_line(const char* text);
std::string label_lines(const ecuio_signal_set_t* set,
                        const ecuio_signal_def_t& d, const std::string& name);
std::string reduce_label(const char* label);
std::string errno_text(int err);
std::string write_reason(int err);
FILE* open_file(const std::string& path);
bool write_bytes(const std::string& path, const std::string& bytes);
bool make_root(const std::string& root, std::string* why);
std::string absolute(const std::string& path);
std::string link_text(int32_t state, uint32_t episodes);
std::string module_text(uint32_t rx, bool closed, bool no_room,
                        uint8_t reasons);
std::string operation_text(const char* id, uint32_t rx, uint64_t bytes,
                           uint32_t end);
std::string envelope_text(uint32_t rx, uint32_t volts_mv, int32_t rpm,
                          int32_t kmh, uint32_t checks);
std::string header_link(int32_t state, uint32_t episodes);
std::string started_lines(std::chrono::system_clock::time_point wall);
bool make_recording_folder(const std::string& root, std::time_t at,
                           const std::string& tail, std::string* leaf,
                           std::string* why, int* from = nullptr);
bool sync_file(FILE* f);

} // namespace ecuio_rec

#endif /* ECUIO_RECORDER_TEXT_H */
