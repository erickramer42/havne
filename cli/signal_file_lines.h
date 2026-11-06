// File/layer: cli/signal_file_lines.h | havne-cli, a signal file's lines (ADR-0051)
// Testing: UNIT (tests/test_cli.cpp, CliSignalFileTest: a URI's profile=,
//          the URI a loaded file opens, the kind words, a unit's
//          conversion, each line's fields); the commands in the golden
//          runs (tests/golden/cli/signals-*.jsonl and stream-signals.jsonl,
//          on tests/fixtures/signals/)
//
// What `havne-cli signals check`, `signals import obdb`, `stream --signals`
// and `sim --signals`
// need besides core's loader (signals.h): the profile a URI selects, and
// the lines that say what the loader read. A file's own signals are its
// defs from ECUIO_USER_SIG_ID_BASE up (signal_sets.h); the defs below
// that are its base set's (extends).

#ifndef HAVNE_CLI_SIGNAL_FILE_LINES_H
#define HAVNE_CLI_SIGNAL_FILE_LINES_H

#include "jsonl.h"
#include "view_lines.h" /* labels_object, labels_bytes */

#include "ecuio/obdb.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace cli {

// A URI's profile=, as core reads it: the last one in the query (after
// the first '?', between '&'s). false for none; true with value, which
// may be empty, for one
inline bool uri_profile(const std::string& uri, std::string* value)
{
    const size_t q = uri.find('?');
    if (q == std::string::npos) {
        return false;
    }
    bool found = false;
    size_t at = q + 1;
    while (at <= uri.size()) {
        size_t end = uri.find('&', at);
        if (end == std::string::npos) {
            end = uri.size();
        }
        if (end - at >= 8 && uri.compare(at, 8, "profile=") == 0) {
            *value = uri.substr(at + 8, end - at - 8);
            found = true;
        }
        at = end + 1;
    }
    return found;
}

// The URI a session opens with a loaded file: the URI as given when it
// selects the file's profile, and with profile=<the file's> added when
// it selects none. false when it selects another, the caller's usage
// error: the session would decode through a set the file isn't
inline bool uri_for_profile(const std::string& uri, const char* profile,
                            std::string* out)
{
    std::string selected;
    if (!uri_profile(uri, &selected)) {
        *out = uri + (uri.find('?') == std::string::npos ? "?" : "&") +
               "profile=" + profile;
        return true;
    }
    if (selected != profile) {
        return false;
    }
    *out = uri;
    return true;
}

// A def's kind as a signal file writes it: number, text, hex, enum or
// computed
inline const char* kind_word(const ecuio_signal_def_t& d)
{
    if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
        return "computed";
    }
    switch (d.kind) {
    case ECUIO_VALUE_TEXT:
        return "text";
    case ECUIO_VALUE_HEX:
        return "hex";
    case ECUIO_VALUE_ENUM:
        return "enum";
    default:
        return "number";
    }
}

// Whether a def's unit converts under imperial (ADR-0022's table); a
// unit outside it is shown as written and never converted
inline bool unit_converts(const ecuio_signal_def_t& d)
{
    ecuio_display_value_t v;
    std::memset(&v, 0, sizeof v);
    return ecuio_display_convert(d.unit, d.decimal_places, 0.0,
                                 ECUIO_UNITS_IMPERIAL,
                                 &v) == ECUIO_DISPLAY_OK &&
           v.converted != 0;
}

// A file's own signal, not its base set's
inline bool own_signal(const ecuio_signal_def_t& d)
{
    return d.id >= ECUIO_USER_SIG_ID_BASE;
}

// Whether a file's own def gets a unit line: a unit, and one that
// doesn't convert
inline bool unit_kept(const ecuio_signal_def_t& d)
{
    return own_signal(d) && d.unit[0] != '\0' && !unit_converts(d);
}

// An enum def's labels in its set's table: count entries, or none
// (nullptr, 0) for another kind or a range the table doesn't hold
inline const ecuio_enum_entry_t* def_labels(const ecuio_signal_set_t& set,
                                            const ecuio_signal_def_t& d,
                                            uint32_t* count)
{
    *count = 0;
    if (d.kind != ECUIO_VALUE_ENUM ||
        (d.flags & ECUIO_DEF_FLAG_COMPUTED) != 0 || set.enums == nullptr ||
        uint32_t{d.table_first} + d.table_count > set.enum_count) {
        return nullptr;
    }
    *count = d.table_count;
    return set.enums + d.table_first;
}

// A computed def's program, or nullptr for another def or an index the
// set's table doesn't hold
inline const ecuio_computed_t* def_program(const ecuio_signal_set_t& set,
                                           const ecuio_signal_def_t& d)
{
    if ((d.flags & ECUIO_DEF_FLAG_COMPUTED) == 0 || set.computed == nullptr ||
        d.table_first >= set.computed_count) {
        return nullptr;
    }
    return set.computed + d.table_first;
}

// The bytes a def's line takes at most: its fields, and its labels
inline size_t def_line_bytes(const ecuio_signal_set_t& set,
                             const ecuio_signal_def_t& d)
{
    uint32_t count = 0;
    def_labels(set, d, &count);
    return 1024 + labels_bytes(count);
}

// {"type":"signal_file", name, profile, signals (the file's own), base
// (its base set's; 0 without extends), ecus [{rx, tx}] (the base's
// first)}
inline void signal_file_line(Line& l, const char* name, const char* profile,
                             const ecuio_signal_set_t& set)
{
    uint32_t own = 0;
    for (uint32_t i = 0; i < set.count; ++i) {
        own += own_signal(set.defs[i]) ? 1u : 0u;
    }
    l.raw("{\"type\":\"signal_file\"");
    l.key("name");
    l.string(name, std::strlen(name));
    l.key("profile");
    l.string(profile, std::strlen(profile));
    l.key("signals");
    l.u64(own);
    l.key("base");
    l.u64(set.count - own);
    l.key("ecus");
    l.ch('[');
    for (uint32_t i = 0; i < set.channel_count; ++i) {
        if (i > 0) {
            l.ch(',');
        }
        l.raw("{\"rx\":");
        l.hex_id(set.channels[i].rx_id);
        l.key("tx");
        l.hex_id(set.channels[i].tx_id);
        l.ch('}');
    }
    l.raw("]}");
}

// {"type":"signal_def", id, name, kind, unit, decimals, group, rank, then
// by kind: a reply's rx, read {service, number} as the file writes them
// and byte (after the echo, as the file's field); a number's and an
// enum's bits, bit (little-endian only), order and signed, a number's
// scale and offset, an enum's labels; a text's bytes (0: to the end);
// a computed signal's inputs [{id, max_age_us}] (the stale age where
// the file gives none)}
inline void def_line(Line& l, const ecuio_signal_set_t& set,
                     const ecuio_signal_def_t& d)
{
    const char* kind = kind_word(d);
    l.raw("{\"type\":\"signal_def\"");
    l.key("id");
    l.u64(d.id);
    l.key("name");
    l.string(d.name, sizeof d.name);
    l.key("kind");
    l.string(kind, std::strlen(kind));
    l.key("unit");
    l.string(d.unit, sizeof d.unit);
    l.key("decimals");
    l.u64(d.decimal_places);
    const char* group = ecuio_signal_group_name(d.group);
    l.key("group");
    l.string(group, std::strlen(group));
    l.key("rank");
    l.u64(d.plot_rank);
    if (const ecuio_computed_t* p = def_program(set, d)) {
        l.key("inputs");
        l.ch('[');
        for (uint32_t i = 0; i < p->input_count && i < 4; ++i) {
            if (i > 0) {
                l.ch(',');
            }
            l.raw("{\"id\":");
            l.u64(p->input_ids[i]);
            l.key("max_age_us");
            l.u64(p->max_age_us[i] != 0 ? p->max_age_us[i]
                                        : ECUIO_VALUE_STALE_US);
            l.ch('}');
        }
        l.raw("]}");
        return;
    }
    l.key("rx");
    l.hex_id(d.message_id);
    // The reply's echo: 41 <PID> (Service 01) or 62 <DID> (22)
    char service[4] = "";
    char number[8] = "";
    unsigned echo = 0;
    if (d.mux_offset == 0 && d.mux_length == 2 && (d.mux_key >> 8) == 0x41) {
        std::snprintf(service, sizeof service, "01");
        std::snprintf(number, sizeof number, "%02X", d.mux_key & 0xFFu);
        echo = 2;
    } else if (d.mux_offset == 0 && d.mux_length == 3 &&
               (d.mux_key >> 16) == 0x62) {
        std::snprintf(service, sizeof service, "22");
        std::snprintf(number, sizeof number, "%04X", d.mux_key & 0xFFFFu);
        echo = 3;
    }
    l.key("read");
    if (echo != 0) {
        l.raw("{\"service\":");
        l.string(service, sizeof service);
        l.key("number");
        l.string(number, sizeof number);
        l.ch('}');
    } else {
        l.raw("null");
    }
    l.key("byte");
    l.u64(d.start_byte >= echo ? d.start_byte - echo : d.start_byte);
    if (d.kind == ECUIO_VALUE_TEXT || d.kind == ECUIO_VALUE_HEX) {
        l.key("bytes");
        l.u64(d.byte_length);
        l.ch('}');
        return;
    }
    const bool le = d.byte_order == ECUIO_BYTE_ORDER_LE;
    l.key("bits");
    l.u64(d.bit_length);
    if (le) {
        l.key("bit");
        l.u64(d.bit_offset);
    }
    l.key("order");
    l.raw(le ? "\"le\"" : "\"be\"");
    l.key("signed");
    l.raw(d.kind == ECUIO_VALUE_INT ? "true" : "false");
    uint32_t count = 0;
    if (d.kind == ECUIO_VALUE_ENUM) {
        const ecuio_enum_entry_t* labels = def_labels(set, d, &count);
        l.key("labels");
        labels_object(l, labels, count);
    } else {
        l.key("scale");
        l.number(d.scale);
        l.key("offset");
        l.number(d.offset);
    }
    l.ch('}');
}

// {"type":"unit", id, name, unit, converts:false}: a file's unit
// outside ADR-0022's table, which loads, shows as written and never
// converts, so a misspelled "kph" doesn't pass for "km/h"
inline void unit_line(Line& l, const ecuio_signal_def_t& d)
{
    l.raw("{\"type\":\"unit\"");
    l.key("id");
    l.u64(d.id);
    l.key("name");
    l.string(d.name, sizeof d.name);
    l.key("unit");
    l.string(d.unit, sizeof d.unit);
    l.key("converts");
    l.raw("false}");
}

// An import note's kind as a word: converted, changed, skipped or file
// (obdb.h's ECUIO_OBDB_NOTE_*); "unknown" for one this CLI doesn't know
inline const char* note_word(uint32_t what)
{
    switch (what) {
    case ECUIO_OBDB_NOTE_CONVERTED:
        return "converted";
    case ECUIO_OBDB_NOTE_CHANGED:
        return "changed";
    case ECUIO_OBDB_NOTE_SKIPPED:
        return "skipped";
    case ECUIO_OBDB_NOTE_FILE:
        return "file";
    default:
        return "unknown";
    }
}

// The bytes a note's line takes at most
inline size_t note_line_bytes(const ecuio_obdb_note_t& n)
{
    return 256 + 6 * (std::strlen(n.signal) + std::strlen(n.key) +
                      std::strlen(n.field) + std::strlen(n.text));
}

// {"type":"import_note", what, line, signal, key, field, text}: one of the
// importer's notes, field by field (obdb.h); signal, key and field "" where
// the note has none
inline void import_note_line(Line& l, const ecuio_obdb_note_t& n)
{
    const char* what = note_word(n.what);
    l.raw("{\"type\":\"import_note\"");
    l.key("what");
    l.string(what, std::strlen(what));
    l.key("line");
    l.u64(n.line);
    l.key("signal");
    l.string(n.signal, std::strlen(n.signal));
    l.key("key");
    l.string(n.key, std::strlen(n.key));
    l.key("field");
    l.string(n.field, std::strlen(n.field));
    l.key("text");
    l.string(n.text, std::strlen(n.text));
    l.ch('}');
}

// {"type":"imported", converted, changed, skipped}: the notes of each kind
// an import gave, the signals in the file being the converted
inline void imported_line(Line& l, uint64_t converted, uint64_t changed,
                          uint64_t skipped)
{
    l.raw("{\"type\":\"imported\"");
    l.key("converted");
    l.u64(converted);
    l.key("changed");
    l.u64(changed);
    l.key("skipped");
    l.u64(skipped);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_SIGNAL_FILE_LINES_H
