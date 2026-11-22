// File/layer: tests/test_cli_pages.cpp | gtest, havne-cli's plugins check of a plugin's pages
// Testing: UNIT: CliPluginTest (test_cli)
//
// cli/page_lines.h over packed plugins built here: a page that reads
// whole, one for one variant, a key no variant has, a cell the layout
// check drops, a page it refuses, two pages of one name, a page of
// built-in signals alone, and the requires_core line for pages.

#include "page_lines.h"

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using Pages = std::vector<std::pair<std::string, std::string>>;

const std::string kA =
    R"({"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", "ecu": "pcm",)"
    R"( "read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}}]})";
const std::string kB =
    R"({"havne_signals": 1, "signals": [{"key": "boost", "name": "Boost", "ecu": "pcm",)"
    R"( "read": {"service": "22", "number": "F40E"}, "field": {"byte": 0}}]})";

std::string cell(const std::string& refs)
{
    return R"({"kind": "digital", "w": 3, "h": 9, "label": "A gauge", "refs": [)" +
           refs + R"(], "unit": "kPa", "options": {"from": 0, "to": 100}})";
}

std::string page(const std::string& form, const std::string& name,
                 const std::string& cells)
{
    return R"({"format": 2, "form": ")" + form +
           R"(", "files": [], "pages": [{"name": ")" + name +
           R"(", "cells": [)" + cells + "]}]}";
}

/* Two variants: base reads oil, turbo oil and boost */
struct Plugin {
    ecuio_plugin_t* p = nullptr;
    explicit Plugin(const Pages& pages)
    {
        std::string m =
            R"({"havne_plugin": 1, "id": "p", "name": "P", "version": "1.0.0", )"
            R"("requires": {"core": "0.1.0"}, "source": {"name": "t"}, )"
            R"("tiers": ["data"], "ecus": [{"id": "pcm", "rx": "7E8"}], )"
            R"("variants": [{"id": "base", "files": ["a.json"]}, )"
            R"({"id": "turbo", "files": ["a.json", "b.json"]}], "pages": [)";
        for (size_t i = 0; i < pages.size(); ++i) {
            m += (i == 0 ? "\"" : ", \"") + pages[i].first + "\"";
        }
        m += R"(], "files": {"a.json": )" + kA + R"(, "b.json": )" + kB;
        for (const auto& pg : pages) {
            m += ", \"" + pg.first + "\": " + pg.second;
        }
        m += "}}";
        EXPECT_EQ(ecuio_plugin_load(m.data(), m.size(), nullptr, nullptr, &p),
                  ECUIO_OK)
            << ecuio_last_error_detail();
    }
    ~Plugin() { ecuio_plugin_free(p); }
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    /* Every page judged in order */
    std::vector<cli::PageCheck> checks() const
    {
        std::vector<cli::PageCheck> out;
        std::vector<std::pair<std::string, std::string>> seen;
        ecuio_plugin_info_t info{};
        info.struct_size = sizeof info;
        EXPECT_EQ(ecuio_plugin_info(p, &info), ECUIO_OK);
        for (uint32_t i = 0; i < info.page_count; ++i) {
            out.push_back(cli::check_page(p, info.variant_count, i, &seen));
        }
        return out;
    }
};

std::string line_of(const cli::PageCheck& c)
{
    std::vector<char> buf(cli::page_line_bytes(c));
    cli::Line l(buf.data(), buf.size());
    cli::page_line(l, c);
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size());
}

using Strings = std::vector<std::string>;

} // namespace

// A page that reads whole and whose keys a variant has passes, naming
// every variant that holds them all
TEST(CliPluginTest, APageThatReadsWholePasses)
{
    const Plugin pl(
        {{"p/phone.json", page("phone", "Engine", cell("\"key:oil\""))}});
    ASSERT_NE(pl.p, nullptr);
    const std::vector<cli::PageCheck> c = pl.checks();
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(line_of(c[0]),
              "{\"type\":\"page\",\"file\":\"p/phone.json\",\"form\":\"phone\","
              "\"name\":\"Engine\",\"ok\":true,\"problem\":\"\",\"cells\":1,"
              "\"dropped_cells\":0,\"variants\":[\"base\",\"turbo\"],"
              "\"unknown_keys\":[],\"reads_plugin\":true,\"words\":null}\n");
}

TEST(CliPluginTest, APageForOneVariantNamesIt)
{
    /* boost first: a variant needs every key, not the last one read */
    const Plugin pl({{"p.json", page("desktop", "Boost",
                                     cell("\"key:boost\", \"id:13001\"") +
                                         ", " + cell("\"key:oil\""))}});
    ASSERT_NE(pl.p, nullptr);
    const cli::PageCheck c = pl.checks().at(0);
    EXPECT_TRUE(c.words.empty()) << c.words;
    EXPECT_EQ(c.form, "desktop");
    EXPECT_EQ(c.variants, Strings{"turbo"});
    EXPECT_TRUE(c.unknown_keys.empty());
}

TEST(CliPluginTest, AKeyNoVariantHasFailsThePage)
{
    const Plugin pl(
        {{"p.json",
          page("phone", "E", cell("\"key:egt\", \"key:oil\", \"key:egt\""))}});
    ASSERT_NE(pl.p, nullptr);
    const cli::PageCheck c = pl.checks().at(0);
    EXPECT_EQ(c.unknown_keys, Strings{"egt"}) << "each key once";
    EXPECT_TRUE(c.variants.empty());
    EXPECT_EQ(c.words,
              "a cell reads key:egt, which no variant of the plugin has");
    EXPECT_NE(line_of(c).find("\"ok\":false"), std::string::npos);
}

// A cell the layout check drops fails the page: a plugin's page loses
// nothing on the way to the user's dashboard
TEST(CliPluginTest, ACellTheLayoutCheckDropsFailsThePage)
{
    const std::string wide =
        R"({"kind": "digital", "w": 99, "h": 9, "label": "Too wide", "refs": ["key:oil"]})";
    const Plugin pl(
        {{"p.json", page("phone", "E", cell("\"key:oil\"") + ", " + wide)}});
    ASSERT_NE(pl.p, nullptr);
    const cli::PageCheck c = pl.checks().at(0);
    EXPECT_EQ(c.verdict.problem, "dropped");
    EXPECT_EQ(c.verdict.dropped_cells, 1);
    EXPECT_EQ(
        c.words,
        "1 cell doesn't read: a field of the wrong kind or past its bound");
}

TEST(CliPluginTest, APageTheLayoutCheckRefusesFails)
{
    std::string cells;
    for (int i = 0; i < 25; ++i) {
        cells += (i == 0 ? "" : ", ") + cell("\"key:oil\"");
    }
    const Plugin pl({{"p.json", page("phone", "E", cells)}});
    ASSERT_NE(pl.p, nullptr) << "core reads no cell, so it takes 25";
    const cli::PageCheck c = pl.checks().at(0);
    EXPECT_EQ(c.verdict.problem, "damaged");
    EXPECT_EQ(c.words, cli::layout::layout_words(c.verdict));
}

// Two pages of one form named alike fail the second; another form's page
// of that name passes
TEST(CliPluginTest, TwoPagesOfOneFormAndNameFailTheSecond)
{
    const Plugin pl(
        {{"a.page", page("phone", "Engine", cell("\"key:oil\""))},
         {"b.page", page("phone", " Engine ", cell("\"key:oil\""))},
         {"c.page", page("desktop", "Engine", cell("\"key:oil\""))}});
    ASSERT_NE(pl.p, nullptr);
    const std::vector<cli::PageCheck> c = pl.checks();
    ASSERT_EQ(c.size(), 3u);
    EXPECT_TRUE(c[0].words.empty()) << c[0].words;
    EXPECT_EQ(c[1].words, "another phone page is named \"Engine\": the list of "
                          "pages would show two the same");
    EXPECT_TRUE(c[2].words.empty()) << c[2].words;
}

// A page of built-in signals alone passes and says it reads none of the
// plugin's
TEST(CliPluginTest, APageOfBuiltInSignalsAloneSaysSo)
{
    const Plugin pl(
        {{"p.json", page("phone", "E", cell("\"id:2001\", \"id:7002\""))}});
    ASSERT_NE(pl.p, nullptr);
    const cli::PageCheck c = pl.checks().at(0);
    EXPECT_TRUE(c.words.empty()) << c.words;
    EXPECT_FALSE(c.reads_plugin);
    EXPECT_EQ(c.variants, (Strings{"base", "turbo"}));
    EXPECT_NE(line_of(c).find("\"reads_plugin\":false"), std::string::npos);
}

TEST(CliPluginTest, PagesUnderAnOlderCoreAreWarned)
{
    const uint16_t eight[3] = {0, 8, 0};
    const uint16_t nine[3] = {0, 9, 0};
    const uint16_t ten[3] = {1, 0, 0};
    EXPECT_TRUE(cli::pages_need_newer_core(eight, 1));
    EXPECT_FALSE(cli::pages_need_newer_core(nine, 1));
    EXPECT_FALSE(cli::pages_need_newer_core(ten, 1));
    EXPECT_FALSE(cli::pages_need_newer_core(eight, 0))
        << "no pages, no warning";
    char buf[1024];
    cli::Line l(buf, sizeof buf);
    cli::pages_core_line(l, eight);
    ASSERT_TRUE(l.finish());
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"requires_core\",\"declared\":\"0.8.0\",\"needs\":"
              "\"0.9.0\",\"warning\":\"the manifest has \\\"pages\\\", which "
              "core reads from 0.9.0 on: raise the manifest's \\\"requires\\\" "
              "core to 0.9.0, so an older Havne names that version instead of "
              "refusing the key\"}\n");
}
