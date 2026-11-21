// File/layer: tests/test_plugin_pages.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginPagesTest (test_signals)
//
// The Dashboard pages a plugin carries (ADR-0052's appendix on pages):
// the manifest's "pages", each page file's envelope, the bounds, and the
// getters, in a folder and packed.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "plugin_rules.h" /* core/src: names_a_device */
#include "plugin_wrap_support.h"

using namespace plugin_wrap_test;

namespace {

const std::string kPhone =
    R"({"format": 2, "form": "phone", "files": [], "pages": [{"name": "Engine", "cells": []}]})";
const std::string kDesktop =
    R"({"format": 2, "form": "desktop", "pages": [{"name": "Engine", "cells": []}]})";

/* A package of data file a.json, one variant reading it, and pages by
 * path; more goes into the manifest before "variants" */
struct Package {
    std::vector<std::pair<std::string, std::string>> pages;
    std::map<std::string, std::string> data = {
        {"a.json", plugin_wrap_test::data(sig("oil", "F40D", "pcm"))}};
    std::string variants = R"({"id": "base", "files": ["a.json"]})";
    std::string more;
    bool list_pages = true;

    std::string manifest(bool packed) const
    {
        std::string m =
            R"({"havne_plugin": 1, "id": "p", "name": "P", "version": "1.0.0", )"
            R"("requires": {"core": "0.1.0"}, "source": {"name": "M"}, )"
            R"("tiers": ["data"], "ecus": [{"id": "pcm", "rx": "7E8"}], )" +
            more;
        if (list_pages && !pages.empty()) {
            m += "\"pages\": [";
            for (size_t i = 0; i < pages.size(); ++i) {
                m += (i == 0 ? "\"" : ", \"") + pages[i].first + "\"";
            }
            m += "], ";
        }
        m += "\"variants\": [" + variants + "]";
        if (packed) {
            m += ", \"files\": {";
            bool first = true;
            for (const auto& d : data) {
                m += (first ? "\"" : ", \"") + d.first + "\": " + d.second;
                first = false;
            }
            for (const auto& p : pages) {
                m += ", \"" + p.first + "\": " + p.second;
            }
            m += "}";
        }
        return m + "}";
    }
};

/* What a load gave: the plugin, or the status and the detail */
struct Loaded {
    ecuio_status_t status = ECUIO_OK;
    std::string detail;
    Plugin plugin;
};

struct Folder {
    std::map<std::string, std::string> files;

    static int read(void* user, const char* path, const char** bytes,
                    size_t* length)
    {
        auto* self = static_cast<Folder*>(user);
        auto it = self->files.find(path);
        if (it == self->files.end()) {
            return 1;
        }
        *bytes = it->second.data();
        *length = it->second.size();
        return 0;
    }
};

Loaded loaded(ecuio_status_t s, ecuio_plugin_t* p)
{
    Loaded l;
    l.status = s;
    l.detail = s == ECUIO_OK ? "" : ecuio_last_error_detail();
    l.plugin.reset(p);
    return l;
}

Loaded load_packed(const Package& k)
{
    const std::string text = k.manifest(true);
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t s =
        ecuio_plugin_load(text.data(), text.size(), nullptr, nullptr, &p);
    return loaded(s, p);
}

Loaded load_folder(const Package& k)
{
    Folder f;
    for (const auto& d : k.data) {
        f.files[d.first] = d.second;
    }
    for (const auto& p : k.pages) {
        f.files[p.first] = p.second;
    }
    const std::string text = k.manifest(false);
    ecuio_plugin_t* p = nullptr;
    const ecuio_status_t s =
        ecuio_plugin_load(text.data(), text.size(), &Folder::read, &f, &p);
    return loaded(s, p);
}

std::string written(const std::string& json)
{
    return ecuio_json::write(parsed(json));
}

ecuio_plugin_page_t page_of(const ecuio_plugin_t* p, uint32_t index)
{
    ecuio_plugin_page_t page{};
    page.struct_size = sizeof page;
    EXPECT_EQ(ecuio_plugin_page(p, index, &page), ECUIO_OK);
    return page;
}

/* text padded with spaces before its last brace to length bytes */
std::string padded(std::string text, size_t length)
{
    EXPECT_LE(text.size(), length);
    text.insert(text.size() - 1, std::string(length - text.size(), ' '));
    return text;
}

std::string hex2(unsigned n)
{
    char buf[3];
    std::snprintf(buf, sizeof buf, "%02X", n & 0xFFu);
    return buf;
}

/* n data files d<i>.json, each one signal of its own, the variant
 * reading the first listed of them */
void with_data_files(Package& k, unsigned n, unsigned listed, size_t length = 0)
{
    k.data.clear();
    std::string files;
    for (unsigned i = 0; i < n; ++i) {
        const std::string name = "d" + std::to_string(i) + ".json";
        std::string d = plugin_wrap_test::data(
            sig("s" + std::to_string(i), "F4" + hex2(i), "pcm"));
        k.data[name] = length == 0 ? d : padded(d, length);
        if (i < listed) {
            files += (i == 0 ? "\"" : ", \"") + name + "\"";
        }
    }
    k.variants = R"({"id": "base", "files": [)" + files + "]}";
}

} // namespace

// The same pages from a folder and its packed form: each one's form and
// path, and its text as core writes it, the same bytes either way
TEST(PluginPagesTest, AFolderAndItsPackedFormGiveTheSamePages)
{
    Package k;
    k.pages = {{"p/phone.json", kPhone}, {"p/desktop.json", kDesktop}};
    Loaded folder = load_folder(k);
    Loaded packed = load_packed(k);
    ASSERT_TRUE(folder.plugin) << folder.detail;
    ASSERT_TRUE(packed.plugin) << packed.detail;
    EXPECT_EQ(info_of(folder.plugin.get()).page_count, 2u);
    EXPECT_EQ(info_of(packed.plugin.get()).page_count, 2u);
    const uint32_t forms[] = {ECUIO_PLUGIN_PAGE_PHONE,
                              ECUIO_PLUGIN_PAGE_DESKTOP};
    for (uint32_t i = 0; i < 2; ++i) {
        for (const Loaded* l : {&folder, &packed}) {
            const ecuio_plugin_page_t page = page_of(l->plugin.get(), i);
            EXPECT_EQ(page.form, forms[i]);
            EXPECT_EQ(std::string(page.path), k.pages[i].first);
            const char* text = ecuio_plugin_page_text(l->plugin.get(), i);
            ASSERT_NE(text, nullptr);
            EXPECT_EQ(std::strlen(text), page.text_bytes);
            EXPECT_EQ(std::string(text), written(k.pages[i].second));
        }
        EXPECT_NE(std::string(ecuio_plugin_page_text(folder.plugin.get(), i)),
                  k.pages[i].second)
            << "the text is core's writing, not the file's bytes";
    }
}

TEST(PluginPagesTest, APluginWithNoPagesListsNone)
{
    Loaded l = load_packed(Package{});
    ASSERT_TRUE(l.plugin) << l.detail;
    EXPECT_EQ(info_of(l.plugin.get()).page_count, 0u);
    EXPECT_EQ(ecuio_plugin_page_text(l.plugin.get(), 0), nullptr);
    ecuio_plugin_page_t page{};
    page.struct_size = sizeof page;
    EXPECT_EQ(ecuio_plugin_page(l.plugin.get(), 0, &page),
              ECUIO_ERR_INVALID_ARGUMENT);
}

// A page that breaks core's envelope refuses the plugin whole, the
// folder's words naming the page's file and its place
TEST(PluginPagesTest, EachEnvelopeRuleRefusesThePluginWhole)
{
    struct row {
        const char* what;
        std::string page;
        const char* words;
    };
    const std::string one = R"("pages": [{"name": "E", "cells": []}])";
    const row rows[] = {
        {"not an object", "[]", "a page file is a list, not an object"},
        {"no format", R"({"form": "phone", )" + one + "}",
         "a page file needs \"format\": 2"},
        {"format 1", R"({"format": 1, "form": "phone", )" + one + "}",
         "a layout file of \"format\" 2"},
        {"format 3", R"({"format": 3, "form": "phone", )" + one + "}",
         "a layout file of \"format\" 2"},
        {"format 2.0", R"({"format": 2.0, "form": "phone", )" + one + "}",
         "a layout file of \"format\" 2"},
        {"format as text", R"({"format": "2", "form": "phone", )" + one + "}",
         "a layout file of \"format\" 2"},
        {"no form", R"({"format": 2, )" + one + "}",
         "a page file needs \"form\": \"phone\" or \"desktop\""},
        {"another form", R"({"format": 2, "form": "tablet", )" + one + "}",
         "\"form\" is \"phone\" or \"desktop\", not 'tablet'"},
        {"a form as a number", R"({"format": 2, "form": 1, )" + one + "}",
         "\"form\" is a number, not a string"},
        {"files listed",
         R"({"format": 2, "form": "phone", "files": [{"name": "x", "sha256": "0"}], )" +
             one + "}",
         "a page file lists no \"files\""},
        {"files as an object",
         R"({"format": 2, "form": "phone", "files": {}, )" + one + "}",
         "\"files\" is an object, not a list"},
        {"no pages", R"({"format": 2, "form": "phone"})",
         "a page file needs \"pages\": one page"},
        {"no page", R"({"format": 2, "form": "phone", "pages": []})",
         "a page file holds one page, not 0"},
        {"two pages",
         R"({"format": 2, "form": "phone", "pages": [{"name": "A"}, {"name": "B"}]})",
         "a page file holds one page, not 2"},
        {"a page that isn't an object",
         R"({"format": 2, "form": "phone", "pages": [1]})",
         "a page is a number, not an object"},
        {"a repeated key",
         R"({"format": 2, "form": "phone", "form": "phone", )" + one + "}",
         "duplicate key 'form'"},
    };
    for (const row& r : rows) {
        Package k;
        k.pages = {{"p/phone.json", r.page}};
        const Loaded folder = load_folder(k);
        EXPECT_EQ(folder.status, ECUIO_ERR_INVALID_ARGUMENT) << r.what;
        EXPECT_EQ(folder.detail.rfind("p/phone.json, line ", 0), 0u)
            << r.what << ": " << folder.detail;
        EXPECT_NE(folder.detail.find(r.words), std::string::npos)
            << r.what << ": " << folder.detail;
        const Loaded packed = load_packed(k);
        EXPECT_EQ(packed.status, ECUIO_ERR_INVALID_ARGUMENT) << r.what;
        EXPECT_EQ(packed.detail.rfind("line ", 0), 0u)
            << r.what << ": " << packed.detail;
        EXPECT_NE(packed.detail.find(r.words), std::string::npos)
            << r.what << ": " << packed.detail;
    }
}

// A key the envelope doesn't list is the layout file's and is kept
TEST(PluginPagesTest, AKeyTheEnvelopeDoesntListIsKept)
{
    Package k;
    k.pages = {{"p.json", R"({"format": 2, "form": "phone", "theme": "dark", )"
                          R"("pages": [{"name": "E", "cells": [], "x": 1}]})"}};
    Loaded l = load_packed(k);
    ASSERT_TRUE(l.plugin) << l.detail;
    const Value page = parsed(ecuio_plugin_page_text(l.plugin.get(), 0));
    EXPECT_EQ(page.find("theme")->text, "dark");
    EXPECT_NE(page.find("pages")->items[0].find("x"), nullptr);
}

TEST(PluginPagesTest, SixteenPagesLoadAndSeventeenDoNot)
{
    Package k;
    for (int i = 0; i < 17; ++i) {
        k.pages.emplace_back("p/" + std::to_string(i) + ".json", kPhone);
    }
    Loaded seventeen = load_packed(k);
    EXPECT_EQ(seventeen.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(seventeen.detail.find("\"pages\" lists more than 16"),
              std::string::npos)
        << seventeen.detail;
    k.pages.pop_back();
    const Loaded sixteen[] = {load_packed(k), load_folder(k)};
    for (const Loaded& l : sixteen) {
        ASSERT_TRUE(l.plugin) << l.detail;
        EXPECT_EQ(info_of(l.plugin.get()).page_count, 16u);
    }
}

// A folder's file at the bound reads and one byte past it is refused;
// the text as written has the same bound
TEST(PluginPagesTest, APageFileIsBoundedAsReadAndAsWritten)
{
    Package k;
    k.pages = {{"p.json", padded(kPhone, ECUIO_PLUGIN_PAGE_MAX_BYTES)}};
    Loaded at = load_folder(k);
    EXPECT_TRUE(at.plugin) << at.detail;
    k.pages[0].second = padded(kPhone, ECUIO_PLUGIN_PAGE_MAX_BYTES + 1);
    Loaded past = load_folder(k);
    EXPECT_EQ(past.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(past.detail.find("page 'p.json' is over 82,400 bytes"),
              std::string::npos)
        << past.detail;

    const auto with_pad = [](size_t n) {
        return R"({"format": 2, "form": "phone", "x-pad": ")" +
               std::string(n, 'a') + R"(", "pages": [{"name": "E"}]})";
    };
    const size_t base = written(with_pad(0)).size();
    ASSERT_LT(base, ECUIO_PLUGIN_PAGE_MAX_BYTES);
    const size_t fits = ECUIO_PLUGIN_PAGE_MAX_BYTES - base;
    k.pages[0].second = with_pad(fits);
    Loaded written_at = load_packed(k);
    ASSERT_TRUE(written_at.plugin) << written_at.detail;
    EXPECT_EQ(page_of(written_at.plugin.get(), 0).text_bytes,
              ECUIO_PLUGIN_PAGE_MAX_BYTES);
    k.pages[0].second = with_pad(fits + 1);
    Loaded written_past = load_packed(k);
    EXPECT_EQ(written_past.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(
        written_past.detail.find("a page file is over 82,400 bytes as written"),
        std::string::npos)
        << written_past.detail;
}

// The count that refuses a large page before writing it never exceeds the
// writing: a page of nested lists and objects, keys and every kind of value
// written to exactly the bound loads (the count is exact for it, so a
// count one byte over refuses it), and a compact one past the bound once
// indented is refused
TEST(PluginPagesTest, APageIsJudgedByItsWritingBeforeItIsWritten)
{
    const auto shaped = [](size_t n) {
        return R"({"format": 2, "form": "desktop", "x-deep": [[1, true, false, null],)"
               R"( {"a": {"b": [{}, [], "cd"]}}], "x-pad": ")" +
               std::string(n, 'a') +
               R"(", "pages": [{"name": "E", "cells": [{"w": 3, "h": 3}]}]})";
    };
    const size_t base = written(shaped(0)).size();
    Package k;
    k.pages = {{"p.json", shaped(ECUIO_PLUGIN_PAGE_MAX_BYTES - base)}};
    Loaded at = load_packed(k);
    ASSERT_TRUE(at.plugin) << at.detail;
    EXPECT_EQ(page_of(at.plugin.get(), 0).text_bytes,
              ECUIO_PLUGIN_PAGE_MAX_BYTES);

    std::string zeros;
    for (int i = 0; i < 20000; ++i) {
        zeros += i == 0 ? "0" : ",0";
    }
    k.pages[0].second = R"({"format": 2, "form": "phone", "x-pad": [)" + zeros +
                        R"(], "pages": [{"name": "E"}]})";
    ASSERT_LT(k.pages[0].second.size(), ECUIO_PLUGIN_PAGE_MAX_BYTES);
    const Loaded both[] = {load_folder(k), load_packed(k)};
    for (const Loaded& l : both) {
        EXPECT_EQ(l.status, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(l.detail.find("a page file is over 82,400 bytes as written"),
                  std::string::npos)
            << l.detail;
    }
}

// A packed "files" holds 64 data files and 16 pages; past either, refused
TEST(PluginPagesTest, APackedPluginHoldsSixtyFourFilesBesideItsPages)
{
    Package k;
    with_data_files(k, 64, 64);
    for (int i = 0; i < 16; ++i) {
        k.pages.emplace_back("p/" + std::to_string(i) + ".json", kPhone);
    }
    Loaded full = load_packed(k);
    EXPECT_TRUE(full.plugin) << full.detail;

    with_data_files(k, 65, 64);
    k.pages.pop_back();
    Loaded data_past = load_packed(k);
    EXPECT_EQ(data_past.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(data_past.detail.find("\"files\" holds more than 64"),
              std::string::npos)
        << data_past.detail;

    k.pages.emplace_back("p/15.json", kPhone);
    Loaded all_past = load_packed(k);
    EXPECT_EQ(all_past.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(all_past.detail.find("\"files\" holds more than 80"),
              std::string::npos)
        << all_past.detail;
}

// Every page's bytes count toward a folder's 16 MiB
TEST(PluginPagesTest, APagesBytesCountTowardThePackage)
{
    Package k;
    with_data_files(k, 15, 15, 1040000);
    Loaded data_alone = load_folder(k);
    ASSERT_TRUE(data_alone.plugin) << data_alone.detail;
    for (int i = 0; i < 16; ++i) {
        k.pages.emplace_back("p/" + std::to_string(i) + ".json",
                             padded(kPhone, ECUIO_PLUGIN_PAGE_MAX_BYTES));
    }
    Loaded with_pages = load_folder(k);
    EXPECT_EQ(with_pages.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(with_pages.detail.find("the package is over 16 MiB"),
              std::string::npos)
        << with_pages.detail;
}

TEST(PluginPagesTest, APageIsAPathListedOnceAndFound)
{
    struct row {
        const char* what;
        std::string more;
        const char* words;
    };
    const row rows[] = {
        {"not a list", R"("pages": "p.json", )",
         "\"pages\" is a string, not a list"},
        {"an empty list", R"("pages": [], )", "\"pages\" lists none"},
        {"a path out of the package", R"("pages": ["../p.json"], )",
         "a page '../p.json' isn't a path in the package"},
        {"listed twice", R"("pages": ["p.json", "p.json"], )",
         "page 'p.json' is listed twice"},
        {"not there", R"("pages": ["q.json"], )", ""},
    };
    for (const row& r : rows) {
        Package k;
        k.pages = {{"p.json", kPhone}};
        k.list_pages = false;
        k.more = r.more;
        const Loaded both[] = {load_folder(k), load_packed(k)};
        for (const Loaded& l : both) {
            EXPECT_EQ(l.status, ECUIO_ERR_INVALID_ARGUMENT) << r.what;
            EXPECT_NE(l.detail.find(r.words), std::string::npos)
                << r.what << ": " << l.detail;
        }
    }
    Package k;
    k.pages = {{"p.json", kPhone}};
    k.list_pages = false;
    k.more = R"("pages": ["q.json"], )";
    EXPECT_NE(load_folder(k).detail.find("page 'q.json' couldn't be read"),
              std::string::npos);
    k.pages.clear();
    EXPECT_NE(load_packed(k).detail.find("page 'q.json' isn't in \"files\""),
              std::string::npos);
}

/* A Windows device name (any case, any extension, in any part) or a part
 * ending in a dot or a space, which Windows drops, opens the device or
 * another file from disk: a folder's read refuses it before the host
 * reads, a page or a data file alike. A packed file's paths are keys in its text, so a
 * plugin installed with one still loads */
TEST(PluginPagesTest, ADeviceNameLoadsPackedAndIsRefusedInAFolder)
{
    const char* const names[] = {"con.json", "p/nul", "Com1.tar.json",
                                 "p./q.json", "AUX.json"};
    for (const char* name : names) {
        SCOPED_TRACE(name);
        Package k;
        k.pages = {{name, kPhone}};
        EXPECT_EQ(load_packed(k).status, ECUIO_OK) << load_packed(k).detail;
        const Loaded folder = load_folder(k);
        EXPECT_EQ(folder.status, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(folder.detail.find("page '" + std::string(name) +
                                     "' is a Windows device name (con, nul, "
                                     "com1, ...) or ends in a dot or a "
                                     "space: a folder's file isn't read by "
                                     "such a name"),
                  std::string::npos)
            << folder.detail;
    }
    Package data;
    data.data = {
        {"data/aux.json", plugin_wrap_test::data(sig("oil", "F40D", "pcm"))}};
    data.variants = R"({"id": "base", "files": ["data/aux.json"]})";
    EXPECT_EQ(load_packed(data).status, ECUIO_OK) << load_packed(data).detail;
    const Loaded folder = load_folder(data);
    EXPECT_EQ(folder.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(folder.detail.find("file 'data/aux.json' is a Windows device "
                                 "name"),
              std::string::npos)
        << folder.detail;

    /* a fixture the host opens itself (plugins check), refused at load */
    Package fixtures;
    fixtures.more = R"("fixtures": ["values.json"], )";
    EXPECT_EQ(load_folder(fixtures).status, ECUIO_OK)
        << load_folder(fixtures).detail;
    fixtures.more = R"("fixtures": ["values.json", "t/Aux.json"], )";
    const Loaded fixture = load_folder(fixtures);
    EXPECT_EQ(fixture.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(fixture.detail.find("fixture 't/Aux.json' is a Windows device "
                                  "name (con, nul, com1, ...) or ends in a "
                                  "dot or a space"),
              std::string::npos)
        << fixture.detail;
}

/* Windows drops a trailing space as it does a dot, so "nul " opens the
 * device. The loader's path rule refuses a space already; the rule that
 * names devices doesn't lean on it */
TEST(PluginPagesTest, ADeviceNameWithATrailingSpaceIsADevice)
{
    for (const char* name : {"nul ", "t/con /v.json", "v.json ", "Com1 ",
                             "a/b./c", "x. "}) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(ecuio_plugin_rules::names_a_device(name));
    }
    for (const char* name : {"nul-x.json", "a b", "t/v.json", "n ul"}) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(ecuio_plugin_rules::names_a_device(name));
    }
}

// A name that only starts like a Windows device is a path like any other
TEST(PluginPagesTest, ANameLikeADevicesIsAPath)
{
    Package k;
    k.pages = {{"console.json", kPhone},
               {"com10.json", kPhone},
               {"lpt/nully.json", kPhone},
               {"p.a/aux_page.json", kPhone}};
    const Loaded both[] = {load_folder(k), load_packed(k)};
    for (const Loaded& l : both) {
        EXPECT_EQ(l.status, ECUIO_OK) << l.detail;
    }
}

// A file is a page or a data file: a variant naming a page is refused,
// and a packed page no "pages" entry names is a file in no variant
TEST(PluginPagesTest, APageIsNeverAVariantsFile)
{
    Package k;
    k.pages = {{"p.json", kPhone}};
    k.variants = R"({"id": "base", "files": ["a.json", "p.json"]})";
    const Loaded both[] = {load_folder(k), load_packed(k)};
    for (const Loaded& l : both) {
        EXPECT_EQ(l.status, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(
            l.detail.find("file 'p.json' is a page, which no variant reads"),
            std::string::npos)
            << l.detail;
    }
    Package stray;
    stray.pages = {{"p.json", kPhone}};
    stray.list_pages = false;
    Loaded l = load_packed(stray);
    EXPECT_EQ(l.status, ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_NE(l.detail.find("file 'p.json' is in no variant"),
              std::string::npos)
        << l.detail;
}

// ecuio_plugin_page: a getter by index, the struct's size checked
TEST(PluginPagesTest, TheGettersFollowTheirContracts)
{
    Package k;
    k.pages = {{"p.json", kPhone}};
    Loaded l = load_packed(k);
    ASSERT_TRUE(l.plugin) << l.detail;
    ecuio_plugin_page_t page;
    std::memset(&page, 0xA5, sizeof page);
    page.struct_size = sizeof page;
    const ecuio_plugin_page_t before = page;
    EXPECT_EQ(ecuio_plugin_page(nullptr, 0, &page), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_page(l.plugin.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_page(l.plugin.get(), 1, &page),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&page, &before, sizeof page), 0) << "out untouched";
    page.struct_size = sizeof page - 1;
    EXPECT_EQ(ecuio_plugin_page(l.plugin.get(), 0, &page),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(page.reserved[0], before.reserved[0]) << "out untouched";
    page.struct_size = sizeof page;
    ASSERT_EQ(ecuio_plugin_page(l.plugin.get(), 0, &page), ECUIO_OK);
    EXPECT_EQ(page.struct_size, sizeof page);
    for (uint32_t r : page.reserved) {
        EXPECT_EQ(r, 0u) << "reserved words are zero";
    }
    EXPECT_EQ(ecuio_plugin_page_text(nullptr, 0), nullptr);
    EXPECT_EQ(ecuio_plugin_page_text(l.plugin.get(), 1), nullptr);
    EXPECT_EQ(info_of(l.plugin.get()).reserved[0], 0u);
}
