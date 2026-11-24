// File/layer: core/src/plugin_publisher.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginPublisherTest (test_signals); FUZZ:
//          fuzz_plugin_manifest
//
// A manifest's "publisher" (format 3, ADR-0066's appendix of 2026-11-23):
// who stands behind a plugin that talks to a module, and on what terms.
// A person reads every one of these texts before consenting, so each is
// plain: no control character (the terms keep line feeds and tabs) and
// no character that shows as nothing or moves the text around it.

#include "plugin_loader.h"

#include "sha256.h"

#include <memory>
#include <string>

namespace ecuio_plugin_loader {

namespace {

/* v's text, lo to hi bytes, plain as the header says; lines lets the
 * terms keep their line feeds and tabs */
const std::string& plain_of(const Value& v, size_t lo, size_t hi,
                            const std::string& what, bool lines)
{
    const std::string& text = text_of(v, lo, hi, what);
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c < 0x20 && !(lines && (c == '\n' || c == '\t'))) || c == 0x7F) {
            refuse(v, what + " holds a control character" +
                          (lines ? ": only line feeds and tabs" : ""));
        }
        if (ecuio_json::invisible_length(text.data() + i, text.size() - i) >
            0) {
            refuse(v, what + " holds a character that shows as nothing or "
                             "moves the text around it");
        }
    }
    return text;
}

} // namespace

/* "publisher": required with the conversation tier in format 3, and
 * format 3's alone */
void loader::read_publisher(const Value& root, ecuio_plugin& p)
{
    const Value* block = root.find("publisher");
    if (block == nullptr) {
        if (p.format >= 3 && (p.tiers & ECUIO_PLUGIN_TIER_CONVERSATION) != 0) {
            refuse(root, "a plugin with the conversation tier names its "
                         "publisher: \"publisher\" with \"name\", "
                         "\"contact\", \"license\" and \"terms\"");
        }
        return;
    }
    if (p.format < 3) {
        refuse(*block, "\"publisher\" is format 3's: its manifest starts "
                       "with \"havne_plugin\": 3");
    }
    expect(*block, Type::object, "\"publisher\"");
    only_keys(*block, {"name", "contact", "license", "terms"}, "\"publisher\"");
    const auto needed = [block](const char* key) -> const Value& {
        const Value* v = block->find(key);
        if (v == nullptr) {
            refuse(*block, std::string("\"publisher\" needs \"") + key + "\"");
        }
        return *v;
    };
    auto out = std::make_unique<publisher>();
    out->name = plain_of(needed("name"), 1, ECUIO_PLUGIN_PUBLISHER_NAME_MAX,
                         "the publisher's name", false);
    out->contact =
        plain_of(needed("contact"), 1, ECUIO_PLUGIN_PUBLISHER_CONTACT_MAX,
                 "the publisher's contact", false);
    out->license =
        plain_of(needed("license"), 1, ECUIO_PLUGIN_PUBLISHER_LICENSE_MAX,
                 "the publisher's license", false);
    out->terms = plain_of(needed("terms"), 1, ECUIO_PLUGIN_TERMS_MAX,
                          "the publisher's terms", true);
    ecuio_sha256::hasher h;
    h.add(out->terms.data(), out->terms.size());
    h.finish(out->terms_sha256);
    p.publisher = std::move(out);
}

} // namespace ecuio_plugin_loader
