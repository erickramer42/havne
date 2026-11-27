#ifndef HAVNE_CLI_NO_ROOM_H
#define HAVNE_CLI_NO_ROOM_H

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cli {

// app/signalmodules.cpp's nameOf: the two front ends name a module alike
inline std::string module_name(const ecuio_signal_set_t* set,
                               const ecuio_signal_file_t* file, uint32_t index)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    if (file != nullptr &&
        ecuio_signal_file_module(file, index, &m) == ECUIO_OK) {
        if (m.name[0] != '\0') {
            return m.name;
        }
        if (m.id[0] != '\0') {
            std::string id = m.id;
            for (char& c : id) {
                c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
            }
            return id;
        }
    }
    char hex[12];
    std::snprintf(hex, sizeof hex, "%X",
                  static_cast<unsigned>(set->channels[index].rx_id));
    return hex;
}

inline std::vector<std::string>
no_room_names(const ecuio_signal_set_t* set, const ecuio_signal_file_t* file,
              const std::vector<uint16_t>& ids,
              const ecuio_channel_support_t* reports, uint32_t channels)
{
    std::vector<std::string> names;
    if (set == nullptr || set->defs == nullptr) {
        return names;
    }
    for (uint32_t i = 0; i < set->count; ++i) {
        const ecuio_signal_def_t& d = set->defs[i];
        if (!ids.empty() &&
            std::find(ids.begin(), ids.end(), d.id) == ids.end()) {
            continue;
        }
        const uint32_t m = ecuio_signal_set_def_module(set, &d);
        if (m < channels && m < set->channel_count &&
            (reports[m].flags & ECUIO_SUPPORT_NO_ROOM) != 0) {
            names.push_back(module_name(set, file, m) + "'s " + d.name);
        }
    }
    return names;
}

inline std::string no_room_text(const std::vector<std::string>& names)
{
    if (names.empty()) {
        return {};
    }
    std::string text = "logging " + std::to_string(names.size()) +
                       (names.size() == 1 ? " signal" : " signals") +
                       " the adapter has no room for: ";
    for (size_t i = 0; i < names.size(); ++i) {
        text += (i != 0 ? ", " : "") + names[i];
    }
    return text + ": their columns stay empty";
}

} // namespace cli

#endif
