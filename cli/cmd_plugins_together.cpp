#include "jsonl.h"
#include "together.h"

#include "commands.h"
#include "common.h"

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <cstring>
#include <string>

namespace cmd {

namespace {

void text_or_null(cli::Line& l, const char* key, const char* text, size_t cap)
{
    l.key(key);
    if (text[0] == '\0') {
        l.raw("null");
    } else {
        l.string(text, cap);
    }
}

void write_part_line(Output& out, const ecuio_signal_file_t* merged,
                     uint32_t index)
{
    ecuio_signal_part_t p{};
    p.struct_size = sizeof p;
    ecuio_signal_source_t src{};
    src.struct_size = sizeof src;
    if (ecuio_signal_file_part(merged, index, &p) != ECUIO_OK ||
        ecuio_signal_file_part_source(merged, index, &src) != ECUIO_OK) {
        return;
    }
    char buf[2048];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"part\"");
    l.key("profile");
    l.string(p.profile, sizeof p.profile);
    l.key("name");
    l.string(p.name, sizeof p.name);
    l.key("format");
    l.u64(p.format);
    text_or_null(l, "plugin", p.plugin_id, sizeof p.plugin_id);
    text_or_null(l, "version", p.plugin_version, sizeof p.plugin_version);
    text_or_null(l, "variant", p.plugin_variant, sizeof p.plugin_variant);
    l.key("signals");
    l.u64(p.signal_count);
    l.key("modules");
    l.u64(p.module_count);
    l.key("share_alike");
    l.raw((src.flags & ECUIO_SOURCE_SHARE_ALIKE) != 0 ? "true" : "false");
    l.ch('}');
    write_line(out, l);
}

void write_module_line(Output& out, const ecuio_signal_file_t* merged,
                       uint32_t index)
{
    ecuio_signal_module_t m{};
    m.struct_size = sizeof m;
    if (ecuio_signal_file_module(merged, index, &m) != ECUIO_OK) {
        return;
    }
    ecuio_signal_part_t p{};
    p.struct_size = sizeof p;
    const bool parted = m.part != ECUIO_SIGNAL_PART_NONE &&
                        ecuio_signal_file_part(merged, m.part, &p) == ECUIO_OK;
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"module\"");
    l.key("rx");
    l.hex_id(m.rx_id);
    l.key("tx");
    l.hex_id(m.tx_id);
    text_or_null(l, "id", m.id, sizeof m.id);
    text_or_null(l, "name", m.name, sizeof m.name);
    text_or_null(l, "part", parted ? p.profile : "", sizeof p.profile);
    l.ch('}');
    write_line(out, l);
}

void write_totals_line(Output& out, const TogetherSet& set)
{
    const ecuio_signal_file_t* f = set.merged;
    const ecuio_signal_set_t* s = ecuio_signal_file_set(f);
    const char* extends = ecuio_signal_file_extends(f);
    const char* base = extends != nullptr && extends[0] != '\0' ? "obd2"
                       : set.flags == ECUIO_MERGE_BASE_MODULES  ? "modules"
                                                                : "";
    char buf[512];
    cli::Line l(buf, sizeof buf);
    l.raw("{\"type\":\"totals\"");
    l.key("profile");
    l.string(set.profile.c_str(), SIZE_MAX);
    text_or_null(l, "base", base, SIZE_MAX);
    l.key("parts");
    l.u64(ecuio_signal_file_part_count(f));
    l.key("modules");
    l.u64(s != nullptr ? s->channel_count : 0);
    l.key("signals");
    l.u64(s != nullptr ? s->count : 0);
    l.key("notes");
    l.u64(ecuio_signal_file_note_count(f));
    l.ch('}');
    write_line(out, l);
}

int read_together_args(int argc, char** argv, Together* t, const char** to)
{
    if (!take_together(argc, argv, 3, t)) {
        return kExitUsage;
    }
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc &&
            *to == nullptr && argv[i + 1][0] != '\0') {
            *to = argv[++i];
        } else {
            std::fprintf(stderr, "havne-cli: unexpected '%s'\n", argv[i]);
            return kExitUsage;
        }
    }
    if (!t->given) {
        std::fprintf(stderr, "havne-cli: plugins together needs --together "
                             "<profiles>\n");
        return kExitUsage;
    }
    return -1;
}

} // namespace

int plugins_together(int argc, char** argv)
{
    Together t;
    const char* to = nullptr;
    const int read = read_together_args(argc, argv, &t, &to);
    if (read >= 0) {
        return read;
    }
    Output out;
    TogetherSet set;
    const int rc = merge_together(out, t, &set);
    if (rc >= 0) {
        return rc;
    }
    if (set.merged == nullptr) {
        std::fprintf(stderr, "havne-cli: obd2 alone is the built-in set: "
                             "nothing to merge\n");
        return kExitUsage;
    }
    write_start(out);
    for (uint32_t i = 0; i < ecuio_signal_file_part_count(set.merged); ++i) {
        write_part_line(out, set.merged, i);
    }
    const ecuio_signal_set_t* s = ecuio_signal_file_set(set.merged);
    for (uint32_t i = 0; s != nullptr && i < s->channel_count; ++i) {
        write_module_line(out, set.merged, i);
    }
    write_totals_line(out, set);
    write_note_lines(out, set.merged);
    size_t length = 0;
    const char* text = ecuio_signal_file_text(set.merged, &length);
    std::string why;
    if (to != nullptr &&
        !write_whole(to, std::string(text, length), &why, "the merged set")) {
        write_error(out, ECUIO_ERR_LOAD_FAILED, why.c_str());
        ecuio_signal_file_free(set.merged);
        return ended(out, kExitFailed);
    }
    ecuio_signal_file_free(set.merged);
    BigLine b(64 + (to != nullptr ? cli::Line::string_bytes(to) : 0));
    b.line.raw("{\"type\":\"end\"");
    b.line.key("out");
    if (to == nullptr) {
        b.line.raw("null");
    } else {
        b.line.string(to, SIZE_MAX);
    }
    b.line.ch('}');
    write_line(out, b.line);
    return ended(out, kExitOk);
}

} // namespace cmd
