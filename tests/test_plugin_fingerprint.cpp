// File/layer: tests/test_plugin_fingerprint.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginFingerprintTest (test_signals)
//
// A plugin's fingerprint (plugin_fingerprint.cpp): one for a folder and
// its packed file, moved by every value and every file, never by a JSON
// file's whitespace, and pinned to values worked out apart from core.

#include "plugin_conversation_support.h"

#include "json.h" /* core/src: packing a folder */

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef ECUIO_PLUGINS_DIR
#error "tests/CMakeLists.txt defines ECUIO_PLUGINS_DIR"
#endif
#ifndef ECUIO_TEST_FIXTURES
#error "tests/CMakeLists.txt defines ECUIO_TEST_FIXTURES"
#endif

namespace {

using conversation_test::Folder;
using conversation_test::Made;
using conversation_test::Plugin;

std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/* Loads a folder on disk, reading each file the loader asks for */
struct DiskFolder {
    std::string root;
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<DiskFolder*>(user);
        std::ifstream in(self->root + "/" + path, std::ios::binary);
        if (!in.good()) {
            return 1;
        }
        std::ostringstream s;
        s << in.rdbuf();
        if (self->files.count(path) == 0) {
            self->asked.emplace_back(path);
        }
        std::string& kept = self->files[path];
        kept = s.str();
        *bytes = kept.data();
        *length = kept.size();
        return 0;
    }

    Plugin load()
    {
        const std::string m = read_file(root + "/manifest.json");
        files["manifest.json"] = m;
        ecuio_plugin_t* p = nullptr;
        EXPECT_EQ(
            ecuio_plugin_load(m.data(), m.size(), &DiskFolder::read, this, &p),
            ECUIO_OK)
            << root << ": " << ecuio_last_error_detail();
        return Plugin(p);
    }
};

/* A folder's files packed as the app and the seeds pack one: the manifest
 * without "fixtures", each file it read under "files", a module as
 * base64 */
std::string pack(const std::map<std::string, std::string>& files,
                 const std::vector<std::string>& asked)
{
    ecuio_json::Value manifest;
    ecuio_json::Error error;
    const std::string& text = files.at("manifest.json");
    EXPECT_TRUE(ecuio_json::parse(text.data(), text.size(), &manifest, &error))
        << error.to_string();
    manifest.members.erase(std::remove_if(manifest.members.begin(),
                                          manifest.members.end(),
                                          [](const ecuio_json::Member& m) {
                                              return m.key == "fixtures";
                                          }),
                           manifest.members.end());
    ecuio_json::Member packed;
    packed.key = "files";
    packed.value.type = ecuio_json::Type::object;
    for (const std::string& path : asked) {
        const std::string& bytes = files.at(path);
        ecuio_json::Member m;
        m.key = path;
        if (path.size() > 5 && path.compare(path.size() - 5, 5, ".wasm") == 0) {
            m.value.type = ecuio_json::Type::object;
            ecuio_json::Member kind;
            kind.key = "kind";
            kind.value.type = ecuio_json::Type::string;
            kind.value.text = "wasm";
            ecuio_json::Member b64;
            b64.key = "base64";
            b64.value.type = ecuio_json::Type::string;
            b64.value.text = conversation_test::base64(
                conversation_test::bytes(bytes.begin(), bytes.end()));
            m.value.members = {kind, b64};
        } else {
            EXPECT_TRUE(
                ecuio_json::parse(bytes.data(), bytes.size(), &m.value, &error))
                << path << ": " << error.to_string();
        }
        packed.value.members.push_back(m);
    }
    manifest.members.push_back(packed);
    return ecuio_json::write(manifest);
}

std::string hex_of(const ecuio_plugin_t* plugin)
{
    ecuio_plugin_fingerprint_t f;
    std::memset(&f, 0xA5, sizeof f);
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_fingerprint(plugin, &f), ECUIO_OK);
    EXPECT_EQ(f.struct_size, sizeof f);
    EXPECT_EQ(f.flags, 0u);
    for (const uint32_t r : f.reserved) {
        EXPECT_EQ(r, 0u);
    }
    return ecuio_sha256::hex(f.fingerprint);
}

std::string hex_of(const Plugin& p)
{
    return p == nullptr ? std::string("(not loaded)") : hex_of(p.get());
}

std::string packed_hex(const std::string& text)
{
    return hex_of(conversation_test::load_packed(text));
}

std::string folder_hex(Folder f)
{
    return hex_of(conversation_test::load_folder(f));
}

const std::vector<std::string> kFolders = {
    ECUIO_PLUGINS_DIR "/example-sim",
    ECUIO_PLUGINS_DIR "/example-car",
    ECUIO_TEST_FIXTURES "/plugins/pages",
    ECUIO_TEST_FIXTURES "/plugins/format2",
    ECUIO_TEST_FIXTURES "/plugins/repeat",
};

} // namespace

TEST(PluginFingerprintTest, EveryFixturesFolderAndPackedFormAgree)
{
    for (const std::string& root : kFolders) {
        DiskFolder folder{root, {}, {}};
        const Plugin p = folder.load();
        ASSERT_NE(p, nullptr) << root;
        EXPECT_EQ(packed_hex(pack(folder.files, folder.asked)), hex_of(p))
            << root;
    }
    /* And a plugin with a module, which packs as base64 */
    const Made made;
    EXPECT_EQ(folder_hex(conversation_test::folder_of(made)),
              packed_hex(made.json()));
}

// The rule in plugin_fingerprint.cpp, worked out in Python's hashlib and
// json over the files on disk, so a change of form is a deliberate one:
//   W(v): json text, two spaces a level, ", " never, ": " after a key,
//         "\n" line ends and one after the root, strings escaped as JSON
//         must and UTF-8 kept
//   h = sha256(b"havne-plugin-fingerprint 1\n")
//   h.update(W(manifest without "files" and "fixtures"))
//   for path in sorted(files, key=bytes): h.update(path + b"\n" +
//       sha256(W(json) or the module's bytes).digest())
// example-car and the pages fixture name their files out of path order;
// conversation.json carries the module.
TEST(PluginFingerprintTest, ThePinnedValuesAreWorkedOutApartFromCore)
{
    const std::map<std::string, std::string> pinned = {
        {ECUIO_PLUGINS_DIR "/example-sim",
         "ea7ac290ececdd41d3cb38b2047279a73dbe48c9932cca4f41b284f54bf03fd0"},
        {ECUIO_PLUGINS_DIR "/example-car",
         "7ac983070c742b4866d2b8cc5f33bc7041e05690a4b3da6a596896b5b10f6a3c"},
        {ECUIO_TEST_FIXTURES "/plugins/pages",
         "a1f52730702a0955c3c9cbf1b25a94c373667cd4f2e43cb4e0ab3980b6ad88bf"},
    };
    for (const auto& [root, want] : pinned) {
        DiskFolder folder{root, {}, {}};
        EXPECT_EQ(hex_of(folder.load()), want) << root;
    }
    EXPECT_EQ(
        packed_hex(read_file(ECUIO_TEST_FIXTURES "/plugins/conversation.json")),
        "23cac2cf723f2eecd98bc783aa3de88373590d076b2b5e4679d0f442aab60e3a");
}

TEST(PluginFingerprintTest, EveryValueAndEveryFileMovesIt)
{
    DiskFolder folder{ECUIO_TEST_FIXTURES "/plugins/pages", {}, {}};
    ASSERT_NE(folder.load(), nullptr);
    const std::string base = packed_hex(pack(folder.files, folder.asked));
    ASSERT_EQ(base.size(), 64u);

    /* One value changed in the manifest, in each data file and in a page */
    std::vector<std::pair<std::string, std::string>> changes;
    for (const std::string& path : folder.asked) {
        changes.emplace_back(path, "");
    }
    changes.emplace_back("manifest.json", "");
    for (const auto& [path, unused] : changes) {
        (void)unused;
        std::map<std::string, std::string> files = folder.files;
        std::string& text = files.at(path);
        /* A page's label, else the first text: a page's first is its form */
        const size_t label = text.find("\"label\": \"");
        const size_t quote =
            label != std::string::npos ? label + 6 : text.find("\": \"");
        ASSERT_NE(quote, std::string::npos) << path;
        text.insert(quote + 4, "x");
        const std::string moved = packed_hex(pack(files, folder.asked));
        EXPECT_EQ(moved.size(), 64u) << path;
        EXPECT_NE(moved, base) << path;
    }

    /* One byte of the module's code */
    Made made;
    const std::string module = packed_hex(made.json());
    wasm_builder::module other = conversation_test::guest();
    other.funcs[3].body = {wasm_builder::I32_CONST, 1};
    made.module = other.build();
    const std::string changed = packed_hex(made.json());
    ASSERT_EQ(changed.size(), 64u);
    EXPECT_NE(changed, module);
}

TEST(PluginFingerprintTest, AJsonFilesWhitespaceAndTheFixturesDontMoveIt)
{
    DiskFolder folder{ECUIO_PLUGINS_DIR "/example-sim", {}, {}};
    const std::string base = hex_of(folder.load());
    ASSERT_EQ(base.size(), 64u);

    /* Each data file written on one line, the manifest naming no fixture */
    Folder flat;
    for (const auto& [path, text] : folder.files) {
        ecuio_json::Value v;
        ecuio_json::Error error;
        ASSERT_TRUE(ecuio_json::parse(text.data(), text.size(), &v, &error));
        std::string one = ecuio_json::write(v);
        one.erase(std::remove(one.begin(), one.end(), '\n'), one.end());
        flat.files[path] = one;
    }
    std::string& m = flat.files["manifest.json"];
    const size_t at = m.find("\"fixtures\"");
    ASSERT_NE(at, std::string::npos);
    const size_t comma = m.rfind(',', at);
    const size_t end = m.find(']', at);
    m.erase(comma, end + 1 - comma); /* the last key, after its comma */
    EXPECT_EQ(folder_hex(flat), base);
}

TEST(PluginFingerprintTest, TheGetterRefusesWhatItCantFill)
{
    const Made made;
    const Plugin p = conversation_test::load_packed(made.json());
    ASSERT_NE(p, nullptr);
    ecuio_plugin_fingerprint_t f;
    std::memset(&f, 0xA5, sizeof f);
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_fingerprint(nullptr, &f), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_fingerprint(p.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    f.struct_size = sizeof f - 1;
    EXPECT_EQ(ecuio_plugin_fingerprint(p.get(), &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(f.struct_size, sizeof f - 1);
    EXPECT_EQ(f.fingerprint[0], 0xA5);
}
