// File/layer: cli/mark_lines.h | havne-cli, C++17, header-only, no core
// Testing: UNIT: CliMarkLinesTest (test_cli)
//
// What a user types into `stream --record` while it runs (ADR-0049
// section 4): each line a marker's label. Standard input gives its bytes
// in whatever pieces the OS reads; this makes lines of them, each without
// its CR or LF. A line past kMax keeps its first kMax bytes, and core
// cuts a label at 1 KB on a character's start.
#ifndef HAVNE_CLI_MARK_LINES_H
#define HAVNE_CLI_MARK_LINES_H

#include <cstddef>
#include <string>

namespace cli {

class MarkLines
{
public:
    static constexpr size_t kMax = 4096;

    /* Takes n bytes; out(line) for each line they complete */
    template <typename Out> void feed(const char* p, size_t n, Out&& out)
    {
        for (size_t i = 0; i < n; ++i) {
            const char c = p[i];
            if (c == '\n') {
                take(out);
            } else if (line_.size() < kMax) {
                line_ += c;
            }
        }
    }

    /* At the end of input: the last line, if it had no newline */
    template <typename Out> void finish(Out&& out)
    {
        if (!line_.empty()) {
            take(out);
        }
    }

private:
    template <typename Out> void take(Out&& out)
    {
        if (!line_.empty() && line_.back() == '\r') {
            line_.pop_back();
        }
        out(line_);
        line_.clear();
    }

    std::string line_;
};

} // namespace cli

#endif
