// File/layer: fuzz/fuzz_same_tables.h | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_signal_merge, fuzz_signal_file
//
// Whether two signal sets hold the same tables, byte for byte: what a
// merged set's text read back, and a format 3 file merged alone, must
// give (ADR-0065 section 9).
#ifndef FUZZ_SAME_TABLES_H
#define FUZZ_SAME_TABLES_H

#include <ecuio/signals.h>

#include <string.h>

inline bool same_tables(const ecuio_signal_set_t* a,
                        const ecuio_signal_set_t* b)
{
    return a->count == b->count && a->channel_count == b->channel_count &&
           a->enum_count == b->enum_count &&
           a->computed_count == b->computed_count &&
           (a->count == 0 ||
            memcmp(a->defs, b->defs, a->count * sizeof *a->defs) == 0) &&
           (a->channel_count == 0 ||
            memcmp(a->channels, b->channels,
                   a->channel_count * sizeof *a->channels) == 0) &&
           (a->enum_count == 0 ||
            memcmp(a->enums, b->enums, a->enum_count * sizeof *a->enums) ==
                0) &&
           (a->computed_count == 0 ||
            memcmp(a->computed, b->computed,
                   a->computed_count * sizeof *a->computed) == 0);
}

#endif /* FUZZ_SAME_TABLES_H */
