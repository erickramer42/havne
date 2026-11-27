#ifndef HAVNE_CLI_TOGETHER_H
#define HAVNE_CLI_TOGETHER_H

#include "common.h"

#include "ecuio/signals.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cmd {

struct Together {
    bool given = false;
    std::vector<std::string> profiles; // --together's, in merge order
    std::vector<const char*> plugins;  // every --plugin
    std::vector<const char*> signals;  // every --signals
};

bool take_together(int& argc, char** argv, int first, Together* t);

// merged is NULL for obd2 alone, whose profile is the built-in "obd2"
struct TogetherSet {
    ecuio_signal_file_t* merged = nullptr;
    std::string profile;
    uint32_t flags = 0;
};

int merge_together(Output& out, const Together& t, TogetherSet* set);
int use_together(const Together& t, const char* uri, std::string* opened);
int take_source(const Together& t, const char* signals, const char* plugin,
                const char** uri, std::string* opened);

extern ecuio_signal_file_t* g_together;

const char* note_kind_word(uint32_t kind);
std::vector<uint16_t> together_left_out();
size_t together_keys_bytes();
void together_keys(cli::Line& l);
void write_note_lines(Output& out, const ecuio_signal_file_t* file);
void write_together_notes(Output& out);

} // namespace cmd

#endif
