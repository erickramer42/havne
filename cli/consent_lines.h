// File/layer: cli/consent_lines.h | havne-cli, a plugin's consent words
//             and lines (ADR-0066's appendix of 2026-11-23)
// Testing: UNIT (tests/test_cli_consent.cpp, CliConsentTest); the
//          commands in the golden runs (tests/golden/cli/plugins-terms.*)
//          and CliConsentTest (tests/cli_consent.py)
//
// What `plugins terms`, `plugins consent` and a run refused for want of
// consent show: Havne's lines from core (ecuio_consent_line, "{name}" and
// "{version}" filled), the publisher block, the terms whole, the
// fingerprint whole (64 digits) or, in a refusal a person reads, its first
// 16; the version-change block when the store holds another version of
// the plugin; the `terms` and `consent` lines (schema 1 growth). Havne
// reads no signature: a publisher who signs publishes it apart, and the
// user checks it with their own tool.

#ifndef HAVNE_CLI_CONSENT_LINES_H
#define HAVNE_CLI_CONSENT_LINES_H

#include "jsonl.h"

#include "ecuio/plugin.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace cli {

// The length of a record's text field, at most its size
inline size_t field_len(const char* s, size_t cap)
{
    const void* nul = std::memchr(s, '\0', cap);
    return nul != nullptr
               ? static_cast<size_t>(static_cast<const char*>(nul) - s)
               : cap;
}

inline std::string hex_text(const uint8_t* b, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += digits[b[i] >> 4];
        out += digits[b[i] & 0xF];
    }
    return out;
}

// A fingerprint typed on the command line: exactly 64 hex digits, either
// case, into out; false for any other text
inline bool parse_fingerprint(const char* text, uint8_t out[32])
{
    if (text == nullptr || std::strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        uint8_t b = 0;
        for (size_t k = 0; k < 2; ++k) {
            const char c = text[i * 2 + k];
            const int v = (c >= '0' && c <= '9')   ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                   : -1;
            if (v < 0) {
                return false;
            }
            b = static_cast<uint8_t>(b * 16 + v);
        }
        out[i] = b;
    }
    return true;
}

// What a consent shows for a plugin: its info, publisher and terms, its
// fingerprint, and the set of Havne's lines its tiers call for
struct ConsentFacts {
    ecuio_plugin_info_t info{};
    ecuio_plugin_publisher_t publisher{};
    bool has_publisher = false;
    std::string terms;
    uint8_t fingerprint[32] = {};
    uint32_t set = 0; // ECUIO_CONSENT_SET_*, 0 for a data plugin
};

// The facts of p; false (a core refusal) only when its info or
// fingerprint can't be read
inline bool consent_facts(const ecuio_plugin_t* p, ConsentFacts* f)
{
    f->info.struct_size = sizeof f->info;
    ecuio_plugin_fingerprint_t print{};
    print.struct_size = sizeof print;
    if (ecuio_plugin_info(p, &f->info) != ECUIO_OK ||
        ecuio_plugin_fingerprint(p, &print) != ECUIO_OK) {
        return false;
    }
    std::memcpy(f->fingerprint, print.fingerprint, sizeof f->fingerprint);
    f->set = (f->info.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) != 0
                 ? ECUIO_CONSENT_SET_CONVERSATION
                 : 0;
    f->publisher.struct_size = sizeof f->publisher;
    f->has_publisher = ecuio_plugin_publisher(p, &f->publisher) == ECUIO_OK;
    if (f->has_publisher) {
        size_t n = 0;
        (void)ecuio_plugin_terms(p, nullptr, 0, &n);
        std::string t(n + 1, '\0');
        if (ecuio_plugin_terms(p, &t[0], t.size(), &n) != ECUIO_OK) {
            return false;
        }
        t.resize(n);
        f->terms = t;
    }
    return true;
}

// text with "{name}" and "{version}" filled once each
inline std::string filled(const char* text, const ecuio_plugin_info_t& info)
{
    std::string out = text;
    for (const auto& pair :
         {std::make_pair(std::string("{name}"), std::string(info.name)),
          std::make_pair(std::string("{version}"),
                         std::string(info.version))}) {
        const size_t at = out.find(pair.first);
        if (at != std::string::npos) {
            out.replace(at, pair.first.size(), pair.second);
        }
    }
    return out;
}

// Havne's lines for f's set, one a line ("" for a data plugin's)
inline std::string havne_lines(const ConsentFacts& f)
{
    std::string out;
    if (f.set == 0) {
        return out;
    }
    for (uint32_t i = 0;; ++i) {
        ecuio_consent_line_t l{};
        l.struct_size = sizeof l;
        if (ecuio_consent_line(f.set, i, &l) != ECUIO_OK) {
            break;
        }
        if (l.heading != nullptr && l.heading[0] != '\0') {
            out += l.heading;
            out += ' ';
        }
        out += filled(l.text, f.info) + "\n";
    }
    return out;
}

// The command a user runs to give consent, the store named when the
// user named one
inline std::string give_command(const char* package, const char* store,
                                const std::string& fingerprint)
{
    std::string out = std::string("havne-cli plugins consent ") + package;
    if (store != nullptr) {
        out += std::string(" --consent-store ") + store;
    }
    return out + " --give " + fingerprint;
}

// `plugins terms`' text: Havne's lines, the publisher block, the terms
// whole, the fingerprint whole and the command that gives consent
inline std::string terms_text(const ConsentFacts& f, const char* package,
                              const char* store)
{
    const std::string print = hex_text(f.fingerprint, 32);
    std::string out = havne_lines(f);
    if (!out.empty()) {
        out += "\n";
    }
    out += std::string("Publisher  ") + f.publisher.name + "\n";
    out += std::string("Contact    ") + f.publisher.contact + "\n";
    out += std::string("License    ") + f.publisher.license + "\n";
    out += std::string("Plugin     ") + f.info.id + " " + f.info.version + "\n";
    out += "Terms\n" + f.terms;
    if (f.terms.empty() || f.terms.back() != '\n') {
        out += "\n";
    }
    out += "\nFingerprint: " + print + "\n";
    out += "To allow this version: " +
           give_command(package, store, "<fingerprint>") + "\n";
    return out;
}

// What a plugin with no publisher says in place of its terms
constexpr const char* kNoPublisher =
    "this plugin names no publisher: it reads signals only";

// {"type":"terms", package, id, version, name, publisher {name, contact,
// license}, terms, lines [Havne's, filled], fingerprint (64)}
inline size_t terms_line_bytes(const ConsentFacts& f)
{
    return 2048 + Line::string_bytes(f.terms.data(), f.terms.size()) +
           Line::string_bytes(havne_lines(f).c_str()) +
           (ECUIO_PLUGIN_PUBLISHER_NAME_MAX +
            ECUIO_PLUGIN_PUBLISHER_CONTACT_MAX +
            ECUIO_PLUGIN_PUBLISHER_LICENSE_MAX + 256) *
               6;
}

inline void publisher_value(Line& l, const ecuio_plugin_publisher_t& p)
{
    l.raw("{\"name\":");
    l.string(p.name, std::strlen(p.name));
    l.key("contact");
    l.string(p.contact, std::strlen(p.contact));
    l.key("license");
    l.string(p.license, std::strlen(p.license));
    l.ch('}');
}

inline void terms_line(Line& l, uint32_t package, const ConsentFacts& f)
{
    l.raw("{\"type\":\"terms\"");
    l.key("package");
    l.u64(package);
    l.key("id");
    l.string(f.info.id, std::strlen(f.info.id));
    l.key("version");
    l.string(f.info.version, std::strlen(f.info.version));
    l.key("name");
    l.string(f.info.name, std::strlen(f.info.name));
    l.key("publisher");
    if (f.has_publisher) {
        publisher_value(l, f.publisher);
    } else {
        l.raw("null");
    }
    l.key("lines");
    l.ch('[');
    const std::string lines = havne_lines(f);
    size_t at = 0;
    bool first = true;
    while (at < lines.size()) {
        const size_t end = lines.find('\n', at);
        if (!first) {
            l.ch(',');
        }
        first = false;
        l.string(lines.data() + at, end - at);
        at = end + 1;
    }
    l.ch(']');
    l.key("terms");
    if (f.has_publisher) {
        l.string_exact(f.terms.data(), f.terms.size());
    } else {
        l.raw("null");
    }
    l.key("fingerprint");
    const std::string print = hex_text(f.fingerprint, 32);
    l.string(print.data(), print.size());
    l.ch('}');
}

// A second as "YYYY-MM-DD" in UTC
inline std::string utc_date(uint64_t second)
{
    const auto t = static_cast<std::time_t>(second);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char out[16] = {};
    std::strftime(out, sizeof out, "%Y-%m-%d", &tm);
    return out;
}

// The words a run refused for want of consent writes on standard error
// (the first 16 digits only, no terms)
inline std::string refusal_text(const ConsentFacts& f, const char* package,
                                const char* store)
{
    const std::string print = hex_text(f.fingerprint, 32);
    std::string out =
        "havne-cli: this plugin needs your consent before it talks to a "
        "car.\n";
    out += std::string("  Publisher  ") + f.publisher.name + "   Contact  " +
           f.publisher.contact + "   License  " + f.publisher.license + "\n";
    out += std::string("  Plugin     ") + f.info.id + " " + f.info.version +
           "   fingerprint " + print.substr(0, 16) + "\n";
    out += "  Havne checks each request against the plugin's declared "
           "services and its own limits. Some requests can lock a module "
           "for a while or for good. The publisher, not the Havne project, "
           "answers for the plugin's content.\n";
    out += std::string("  Read the terms:  havne-cli plugins terms ") +
           package + "\n";
    out += "  Then allow this version:  " +
           give_command(package, store, "<fingerprint>") + "\n";
    return out;
}

// The version-change block (the words' section 2, plain): what the store's
// newest record of the plugin's id says against f
inline std::string version_change_text(const ConsentFacts& f,
                                       const ecuio_consent_record_t& before,
                                       const char* package, const char* store)
{
    std::string out = "havne-cli: this plugin has changed since you allowed "
                      "it.\n  You allowed ";
    out += std::string(f.info.name) + " " + before.version + " on " +
           utc_date(before.given_utc) + ". ";
    if (std::strcmp(before.version, f.info.version) == 0) {
        out += std::string("This file is not the same plugin: its contents "
                           "differ, though it still says ") +
               f.info.version + ".\n";
    } else {
        out += std::string("This is ") + f.info.version + ".\n";
    }
    if (std::strcmp(before.publisher, f.publisher.name) == 0) {
        out += std::string("  Same publisher: ") + f.publisher.name + ".\n";
    } else {
        out += std::string("  The publisher is different. Before: ") +
               before.publisher + ". Now: " + f.publisher.name + ".\n";
    }
    out += std::memcmp(before.terms_sha256, f.publisher.terms_sha256, 32) == 0
               ? "  The terms are the same as before.\n"
               : "  The terms have changed. Read them: havne-cli plugins "
                 "terms " +
                     std::string(package) + "\n";
    out += "  Havne can't tell you what else changed. The plugin may now "
           "send other requests.\n";
    out += "  Then allow this version:  " +
           give_command(package, store, "<fingerprint>") + "\n";
    return out;
}

// {"type":"consent", id, version, fingerprint (64), publisher, given_utc}:
// one record of `plugins consent --list`
inline void consent_line(Line& l, const ecuio_consent_record_t& r)
{
    l.raw("{\"type\":\"consent\"");
    l.key("id");
    l.string(r.id, field_len(r.id, sizeof r.id));
    l.key("version");
    l.string(r.version, field_len(r.version, sizeof r.version));
    l.key("fingerprint");
    const std::string print = hex_text(r.fingerprint, 32);
    l.string(print.data(), print.size());
    l.key("publisher");
    l.string(r.publisher, field_len(r.publisher, sizeof r.publisher));
    l.key("given_utc");
    l.u64(r.given_utc);
    l.ch('}');
}

// The session line's "consent" value: {"fingerprint" (64), "given_utc"}
inline void session_consent(Line& l, const uint8_t fingerprint[32],
                            uint64_t given_utc)
{
    l.raw("{\"fingerprint\":");
    const std::string print = hex_text(fingerprint, 32);
    l.string(print.data(), print.size());
    l.key("given_utc");
    l.u64(given_utc);
    l.ch('}');
}

} // namespace cli

#endif // HAVNE_CLI_CONSENT_LINES_H
