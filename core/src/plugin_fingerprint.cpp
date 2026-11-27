// File/layer: core/src/plugin_fingerprint.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginFingerprintTest (test_signals)
//
// A plugin's fingerprint (ADR-0066's appendix of 2026-11-23): the one
// SHA-256 that names what a person consents to, the same for a folder and
// its packed file. The rule:
//
//   fingerprint = SHA-256( "havne-plugin-fingerprint 1\n"
//                          || W(the manifest without "files" and "fixtures")
//                          || for each file, in byte order of its path:
//                               path || "\n" || SHA-256(its bytes) )
//
// W is ecuio_json::write. A file is each data file a variant reads, each
// page and the conversation's module; a data file's and a page's bytes
// are W of the parsed JSON, the module's its own. So whitespace inside a
// JSON file changes nothing, and every value and every module byte does.
// The fixtures run from a folder and never enter a packed file, so they
// are no part of it.

#include "plugin_loader.h"

#include "sha256.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_plugin_loader {

namespace {

constexpr char kDomain[] = "havne-plugin-fingerprint 1\n";

struct entry {
    std::string path;
    uint8_t sha256[32];
};

entry entry_of(const std::string& path, const void* bytes, size_t n)
{
    entry e{path, {}};
    ecuio_sha256::hasher h;
    h.add(bytes, n);
    h.finish(e.sha256);
    return e;
}

entry entry_of(const std::string& path, const std::string& text)
{
    return entry_of(path, text.data(), text.size());
}

} // namespace

void loader::read_fingerprint(const Value& root, ecuio_plugin& p) const
{
    Value manifest;
    manifest.type = Type::object;
    for (const Member& m : root.members) {
        if (m.key != "files" && m.key != "fixtures") {
            manifest.members.push_back(m);
        }
    }

    std::vector<entry> entries;
    entries.reserve(files_.size() + p.pages.size() + 1);
    for (const data_file& f : files_) {
        entries.push_back(entry_of(f.path, ecuio_json::write(*f.root)));
    }
    for (const page& pg : p.pages) {
        entries.push_back(entry_of(pg.path, pg.text));
    }
    if (p.conversation != nullptr) {
        const Value* code = root.find("conversation")->find("code");
        const std::vector<uint8_t>& bytes = p.conversation->code;
        entries.push_back(
            entry_of(code->find("file")->text, bytes.data(), bytes.size()));
    }
    for (const ecuio_operation::plan& op : p.operations) {
        if (op.has_kernel) {
            entries.push_back(entry_of(op.helper.file, op.helper.blob.data(),
                                       op.helper.blob.size()));
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const entry& a, const entry& b) { return a.path < b.path; });

    ecuio_sha256::hasher h;
    h.add(kDomain, sizeof kDomain - 1);
    const std::string text = ecuio_json::write(manifest);
    h.add(text.data(), text.size());
    for (const entry& e : entries) {
        h.add(e.path.data(), e.path.size());
        h.add("\n", 1);
        h.add(e.sha256, sizeof e.sha256);
    }
    h.finish(p.fingerprint);
}

} // namespace ecuio_plugin_loader
