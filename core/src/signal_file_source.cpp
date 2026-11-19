// File/layer: core/src/signal_file_source.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileTest, PluginTest (test_signals);
//          FUZZ: fuzz_signal_file
//
// A signal file's "source" (ADR-0064): its rules, what
// ecuio_signal_file_source gives, and the share-alike reading of a license.

#include "ecuio/signals.h"
#include "json.h"
#include "signal_file_build.h"

#include <algorithm>
#include <string>

namespace ecuio_sf {

using ecuio_json::Type;
using ecuio_json::Value;

void read_source(const Value& source)
{
    expect(source, Type::object, "\"source\"");
    only_keys(source, {"name", "url", "commit", "license", "modified"},
              "\"source\"");
    const Value* source_name = source.find("name");
    if (source_name == nullptr) {
        refuse(source, "\"source\" needs a \"name\"");
    }
    text_of(*source_name, 1, ECUIO_SOURCE_TEXT_MAX, "the source's name");
    for (const char* k : {"url", "commit", "license", "modified"}) {
        if (const Value* v = source.find(k)) {
            text_of(*v, 1, ECUIO_SOURCE_TEXT_MAX,
                    std::string("the source's ") + k);
        }
    }
}

/* ASCII alone, as profile_of reads a name: the process's locale (Qt and
 * Python set one) would make a high byte a letter in some */
bool share_alike(const std::string& license)
{
    std::string t;
    for (char c : license) {
        t += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    const auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    /* "CC", "BY", ["NC",] "SA" from at, each part after a space or a
     * hyphen; the end of the name not inside a longer word */
    const auto name_at = [&](size_t at) {
        size_t i = at;
        const auto part = [&](const char* p, bool first) {
            if (!first) {
                if (i >= t.size() || (t[i] != '-' && t[i] != ' ')) {
                    return false;
                }
                ++i;
            }
            if (t.compare(i, 2, p) != 0) {
                return false;
            }
            i += 2;
            return true;
        };
        if (!part("CC", true) || !part("BY", false)) {
            return false;
        }
        const size_t before_nc = i;
        if (!part("NC", false)) {
            i = before_nc;
        }
        return part("SA", false) && (i == t.size() || !alnum(t[i]));
    };
    for (size_t at = t.find("CC"); at != std::string::npos;
         at = t.find("CC", at + 1)) {
        if ((at == 0 || (!alnum(t[at - 1]) && t[at - 1] != '-')) &&
            name_at(at)) {
            return true;
        }
    }
    return false;
}

void take_source(const Value& source, ecuio_signal_source_t& out)
{
    out = ecuio_signal_source_t{};
    const auto put = [&source](const char* key, char* to) {
        if (const Value* v = source.find(key)) {
            /* read_source bounds it at ECUIO_SOURCE_TEXT_MAX: the NUL fits */
            std::copy_n(v->text.data(),
                        std::min<size_t>(v->text.size(), ECUIO_SOURCE_TEXT_MAX),
                        to);
        }
    };
    put("name", out.name);
    put("url", out.url);
    put("commit", out.commit);
    put("license", out.license);
    put("modified", out.modified);
    out.flags = share_alike(out.license) ? ECUIO_SOURCE_SHARE_ALIKE : 0u;
}

} // namespace ecuio_sf
