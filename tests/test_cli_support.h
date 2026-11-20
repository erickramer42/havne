// File/layer: tests/test_cli_support.h | havne-cli's line formatting and output ring (ADR-0031)
// Testing: UNIT (test_cli): this file's suites

#pragma once

#include "can_line.h"
#include "clear_run.h"
#include "diagnostics_line.h"
#include "elm327_emulator.h"
#include "elm327_lines.h"
#include "info_lines.h"
#include "jsonl.h"
#include "line_ring.h"
#include "mark_lines.h"
#include "plugin_lines.h"
#include "recording_signals.h"
#include "refused_signals.h"
#include "scan_line.h"
#include "signal_file_lines.h"
#include "sim_line.h"
#include "sleep_watch.h"
#include "timing_line.h"
#include "view_frames.h"
#include "view_lines.h"
#include "vin_line.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

inline std::vector<std::string> lines(const char* p, size_t n)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += p[i];
        }
    }
    EXPECT_TRUE(cur.empty()) << "a line without its \\n";
    return out;
}

inline std::string text_of(cli::Line& l)
{
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size());
}

inline ecuio_dropped_request_t dropped(uint8_t service, uint16_t key, uint8_t length,
                                uint8_t reason, uint8_t nrc)
{
    ecuio_dropped_request_t d{};
    d.request[0] = service;
    if (length == 2) {
        d.request[1] = static_cast<uint8_t>(key);
    } else {
        d.request[1] = static_cast<uint8_t>(key >> 8);
        d.request[2] = static_cast<uint8_t>(key);
    }
    d.length = length;
    d.reason = reason;
    d.nrc = nrc;
    return d;
}

inline ecuio_channel_support_t
report(uint32_t rx, std::initializer_list<ecuio_dropped_request_t> drops)
{
    ecuio_channel_support_t s{};
    s.rx_id = rx;
    s.generation = 1;
    for (const ecuio_dropped_request_t& d : drops) {
        s.dropped_list[s.dropped++] = d;
    }
    return s;
}

/* Each line's type, and the lines, as run_clear wrote them */
struct Written {
    std::vector<std::string> lines;
    bool flush_ok = true;
    int flushes = 0;
    cli::ClearIo io()
    {
        cli::ClearIo io;
        io.write = [this](cli::Line& l) {
            lines.emplace_back(l.data(), l.size());
        };
        io.flush = [this] {
            ++flushes;
            return flush_ok;
        };
        io.stop = [] { return false; };
        return io;
    }
    std::vector<std::string> types() const
    {
        std::vector<std::string> t;
        for (const std::string& l : lines) {
            const size_t a = l.find("\"type\":\"") + 8;
            t.push_back(l.substr(a, l.find('"', a) - a));
        }
        return t;
    }
};

} // namespace
