// File/layer: tests/test_plugin_conversation.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginConversationTest (test_signals)
//
// The plugin loader's reading of the conversation tier (ADR-0066,
// "havne_plugin" 2; core/src/plugin_conversation.cpp): the group and its
// module read back through plugin.h, packed and as a folder, each bound's
// own value, and a put and a drop refused. The rules' refusals are
// test_plugin_conversation_rules.cpp's. Nothing here runs a module.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wasm_check.h" /* core/src: MODULE_MAX_BYTES */

#include "plugin_conversation_support.h"

namespace {

using conversation_test::conversation_of;
using conversation_test::ecu_of;
using conversation_test::Folder;
using conversation_test::folder_of;
using conversation_test::load_folder;
using conversation_test::load_packed;
using conversation_test::Made;
using conversation_test::Plugin;
using conversation_test::sha_of;
using wasm_builder::bytes;

/* What the plugin Made's defaults describe holds, read back */
void expect_the_default(const ecuio_plugin_t* p, const bytes& module)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(p, &info), ECUIO_OK);
    EXPECT_EQ(info.tiers,
              ECUIO_PLUGIN_TIER_DATA | ECUIO_PLUGIN_TIER_CONVERSATION);

    const ecuio_plugin_conversation_t c = conversation_of(p);
    EXPECT_EQ(c.struct_size, sizeof c);
    EXPECT_EQ(c.code_bytes, module.size());
    char hex[65] = {};
    for (size_t i = 0; i < sizeof c.code_sha256; ++i) {
        std::snprintf(hex + 2 * i, 3, "%02x", unsigned{c.code_sha256[i]});
    }
    EXPECT_EQ(std::string(hex), sha_of(module));
    EXPECT_EQ(c.budget, 250000u);
    ASSERT_EQ(c.ecu_count, 2u);
    ASSERT_EQ(c.service_count, 4u);
    ASSERT_EQ(c.level_count, 2u);
    EXPECT_EQ(c.reserved[0], 0u);
    EXPECT_EQ(c.reserved[1], 0u);

    /* the ECUs in the group's order, each by its reply ID */
    const ecuio_plugin_conversation_ecu_t tcm = ecu_of(p, 0);
    EXPECT_EQ(tcm.struct_size, sizeof tcm);
    EXPECT_STREQ(tcm.id, "tcm");
    for (size_t k = 3; k < sizeof tcm.id; ++k) {
        EXPECT_EQ(tcm.id[k], '\0') << k;
    }
    EXPECT_EQ(tcm.rx_id, 0x7E9u);
    EXPECT_EQ(tcm.tx_id, 0x7E1u) << "the standard's request ID for 7E9";
    EXPECT_EQ(tcm.flags, 0u);
    for (const uint32_t r : tcm.reserved) {
        EXPECT_EQ(r, 0u);
    }
    const ecuio_plugin_conversation_ecu_t pcm = ecu_of(p, 1);
    EXPECT_STREQ(pcm.id, "pcm");
    EXPECT_EQ(pcm.rx_id, 0x7E8u);
    EXPECT_EQ(pcm.tx_id, 0x7E0u);

    const struct {
        uint8_t sid;
        std::vector<uint8_t> subs;
    } services[] = {{0x10, {0x03}},
                    {0x22, {}},
                    {0x27, {0x01, 0x02, 0x03, 0x04}},
                    {0x3E, {0x00}}};
    for (uint32_t i = 0; i < 4; ++i) {
        ecuio_plugin_service_t s;
        std::memset(&s, 0xA5, sizeof s);
        ASSERT_EQ(ecuio_plugin_service(p, i, &s), ECUIO_OK);
        EXPECT_EQ(s.sid, services[i].sid);
        ASSERT_EQ(s.sub_count, services[i].subs.size()) << "service " << i;
        for (size_t k = 0; k < sizeof s.subs; ++k) {
            EXPECT_EQ(s.subs[k],
                      k < services[i].subs.size() ? services[i].subs[k] : 0)
                << "service " << i << ", sub " << k;
        }
        EXPECT_EQ(s.reserved[0], 0u);
        EXPECT_EQ(s.reserved[1], 0u);
    }

    ecuio_plugin_security_level_t l;
    std::memset(&l, 0xA5, sizeof l);
    ASSERT_EQ(ecuio_plugin_security_level(p, 0, &l), ECUIO_OK);
    EXPECT_EQ(l.rx_id, 0x7E8u);
    EXPECT_EQ(l.level, 0x01);
    EXPECT_EQ(l.seed_bytes, 4);
    EXPECT_EQ(l.key_bytes, 4);
    EXPECT_EQ(l.reserved0, 0);
    EXPECT_EQ(l.delay_ms, 10000u) << "the default";
    EXPECT_EQ(l.key_window_ms, 1000u) << "the default";
    for (const uint32_t r : l.reserved) {
        EXPECT_EQ(r, 0u);
    }
    ASSERT_EQ(ecuio_plugin_security_level(p, 1, &l), ECUIO_OK);
    EXPECT_EQ(l.rx_id, 0x7E9u);
    EXPECT_EQ(l.level, 0x03);
    EXPECT_EQ(l.seed_bytes, 16);
    EXPECT_EQ(l.key_bytes, 16);
    EXPECT_EQ(l.delay_ms, 30000u);
    EXPECT_EQ(l.key_window_ms, 250u);
}

} // namespace

TEST(PluginConversationTest, APackedPluginsConversationReadsBack)
{
    const Made m;
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_packed(m.json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_default(p.get(), m.module);
}

TEST(PluginConversationTest, AFolderReadsItsModuleThroughTheCallback)
{
    const Made m;
    Folder f = folder_of(m);
    ecuio_status_t s = ECUIO_OK;
    Plugin p = load_folder(f, &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_default(p.get(), m.module);
    EXPECT_EQ(f.asked, (std::vector<std::string>{"a.json", "guest.wasm"}))
        << "each file asked once, the module after the variants' files";
}

TEST(PluginConversationTest, TheLeastGroupTakesItsDefaults)
{
    Made m;
    m.group.budget.clear();
    m.group.security.clear();
    m.group.ecus = R"(["abs"])";
    m.group.services = R"([{"sid": "19"}])";
    Plugin p = load_packed(m.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    const ecuio_plugin_conversation_t c = conversation_of(p.get());
    EXPECT_EQ(c.budget, ECUIO_PLUGIN_BUDGET_MAX);
    EXPECT_EQ(c.ecu_count, 1u);
    EXPECT_EQ(c.service_count, 1u);
    EXPECT_EQ(c.level_count, 0u);
    const ecuio_plugin_conversation_ecu_t abs = ecu_of(p.get(), 0);
    EXPECT_STREQ(abs.id, "abs");
    EXPECT_EQ(abs.rx_id, 0x7EAu) << "an ECU no signal reads";
}

TEST(PluginConversationTest, EachBoundsOwnValueLoads)
{
    /* 32 ECUs, security access by its 14 sub-functions, 32 levels: two
     * for each of 16 ECUs, each level number on several ECUs */
    Made m;
    conversation_test::all_32_ecus(m);
    m.group.services =
        R"([{"sid": "10", "sub": ["03"]}, {"sid": "19"}, {"sid": "22"}, )"
        R"({"sid": "2A"}, {"sid": "27", "sub": ["01", "02", "03", "04", "05", )"
        R"("06", "07", "08", "09", "0A", "0B", "0C", "0D", "0E"]}, )"
        R"({"sid": "2C", "sub": ["01", "03"]}, {"sid": "3E", "sub": ["00"]}])";
    std::string levels = "[";
    for (int i = 0; i < 32; ++i) {
        char level[224];
        const std::string id = conversation_test::ecu_id(i / 2);
        std::snprintf(
            level, sizeof level,
            R"({"ecu": "%s", "level": "%02X", "seed_bytes": %d, )"
            R"("key_bytes": %d, "delay_ms": %d, "key_window_ms": %d})",
            id.c_str(), 2 * (i % 7) + 1, i == 0 ? 1 : 255, i == 0 ? 1 : 58,
            i == 0 ? 1000 : 3600000, i == 0 ? 50 : 4000);
        levels += std::string(i > 0 ? ", " : "") + level;
    }
    m.group.security = levels + "]";
    m.group.budget = "1";
    Plugin p = load_packed(m.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();

    const ecuio_plugin_conversation_t c = conversation_of(p.get());
    EXPECT_EQ(c.budget, 1u);
    ASSERT_EQ(c.ecu_count, 32u);
    ASSERT_EQ(c.service_count, 7u);
    ASSERT_EQ(c.level_count, 32u);
    const ecuio_plugin_conversation_ecu_t last = ecu_of(p.get(), 31);
    EXPECT_STREQ(last.id, "e31");
    EXPECT_EQ(last.rx_id, 0x71Fu);
    EXPECT_EQ(last.tx_id, 0x61Fu) << "the manifest's own request ID";
    const ecuio_plugin_conversation_ecu_t longest = ecu_of(p.get(), 1);
    EXPECT_EQ(std::string(longest.id, sizeof longest.id),
              std::string(31, 'x') + std::string(1, '\0'))
        << "an id of 31 characters, whole and ended";
    ecuio_plugin_service_t s{};
    ASSERT_EQ(ecuio_plugin_service(p.get(), 4, &s), ECUIO_OK);
    EXPECT_EQ(s.sid, 0x27);
    ASSERT_EQ(s.sub_count, 14);
    EXPECT_EQ(s.subs[0], 0x01);
    EXPECT_EQ(s.subs[13], 0x0E);
    ASSERT_EQ(ecuio_plugin_service(p.get(), 6, &s), ECUIO_OK);
    EXPECT_EQ(s.sid, 0x3E);

    ecuio_plugin_security_level_t l{};
    ASSERT_EQ(ecuio_plugin_security_level(p.get(), 0, &l), ECUIO_OK);
    EXPECT_EQ(l.rx_id, 0x700u);
    EXPECT_EQ(l.level, 0x01);
    EXPECT_EQ(l.seed_bytes, 1);
    EXPECT_EQ(l.key_bytes, 1);
    EXPECT_EQ(l.delay_ms, 1000u);
    EXPECT_EQ(l.key_window_ms, 50u);
    ASSERT_EQ(ecuio_plugin_security_level(p.get(), 1, &l), ECUIO_OK);
    EXPECT_EQ(l.rx_id, 0x700u) << "a second level of one ECU";
    EXPECT_EQ(l.level, 0x03);
    ASSERT_EQ(ecuio_plugin_security_level(p.get(), 31, &l), ECUIO_OK);
    EXPECT_EQ(l.rx_id, 0x70Fu);
    EXPECT_EQ(l.level, 0x07) << "a level number other ECUs have too";
    EXPECT_EQ(l.seed_bytes, 255);
    EXPECT_EQ(l.key_bytes, ECUIO_PLUGIN_KEY_MAX);
    EXPECT_EQ(l.delay_ms, 3600000u);
    EXPECT_EQ(l.key_window_ms, 4000u);

    m.group.budget = "1000000";
    Plugin most = load_packed(m.json());
    ASSERT_NE(most, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(conversation_of(most.get()).budget, ECUIO_PLUGIN_BUDGET_MAX);
}

TEST(PluginConversationTest, Format2WithTheDataTierAloneIsAPluginAsBefore)
{
    Made m;
    m.tiers = R"(["data"])";
    m.has_group = false;
    m.has_binary = false;
    Plugin p = load_packed(m.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(p.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.tiers, ECUIO_PLUGIN_TIER_DATA);
    EXPECT_EQ(info.variant_count, 1u);
}

TEST(PluginConversationTest, TheGettersRefuseWhatTheyCantFill)
{
    const Made m;
    Plugin p = load_packed(m.json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    Made plain;
    plain.format = "1";
    plain.tiers = R"(["data"])";
    plain.has_group = false;
    plain.has_binary = false;
    Plugin data_only = load_packed(plain.json());
    ASSERT_NE(data_only, nullptr) << ecuio_last_error_detail();

    ecuio_plugin_conversation_t c;
    ecuio_plugin_conversation_t untouched;
    std::memset(&c, 0xA5, sizeof c);
    c.struct_size = sizeof c;
    untouched = c;
    EXPECT_EQ(ecuio_plugin_conversation(nullptr, &c), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_conversation(p.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_conversation(data_only.get(), &c),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a plugin without the tier";
    EXPECT_EQ(std::memcmp(&c, &untouched, sizeof c), 0);
    c.struct_size = sizeof c - 1;
    untouched = c;
    EXPECT_EQ(ecuio_plugin_conversation(p.get(), &c),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a struct smaller than this core's";
    EXPECT_EQ(std::memcmp(&c, &untouched, sizeof c), 0);
    c.struct_size = sizeof c + 8; /* a later caller's larger struct */
    EXPECT_EQ(ecuio_plugin_conversation(p.get(), &c), ECUIO_OK);
    EXPECT_EQ(c.struct_size, sizeof c);

    ecuio_plugin_conversation_ecu_t e;
    ecuio_plugin_conversation_ecu_t e_untouched;
    std::memset(&e, 0xA5, sizeof e);
    e.struct_size = sizeof e;
    e_untouched = e;
    EXPECT_EQ(ecuio_plugin_conversation_ecu(nullptr, 0, &e),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_conversation_ecu(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_conversation_ecu(p.get(), 2, &e),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_conversation_ecu(data_only.get(), 0, &e),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&e, &e_untouched, sizeof e), 0);
    e.struct_size = sizeof e - 1;
    e_untouched = e;
    EXPECT_EQ(ecuio_plugin_conversation_ecu(p.get(), 0, &e),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a struct smaller than this core's";
    EXPECT_EQ(std::memcmp(&e, &e_untouched, sizeof e), 0);
    e.struct_size = sizeof e + 8; /* a later caller's larger struct */
    EXPECT_EQ(ecuio_plugin_conversation_ecu(p.get(), 0, &e), ECUIO_OK);
    EXPECT_EQ(e.struct_size, sizeof e);

    ecuio_plugin_service_t s;
    ecuio_plugin_service_t s_untouched;
    std::memset(&s, 0xA5, sizeof s);
    s_untouched = s;
    EXPECT_EQ(ecuio_plugin_service(nullptr, 0, &s), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_service(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_service(p.get(), 4, &s), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_service(data_only.get(), 0, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&s, &s_untouched, sizeof s), 0);

    ecuio_plugin_security_level_t l;
    ecuio_plugin_security_level_t l_untouched;
    std::memset(&l, 0xA5, sizeof l);
    l_untouched = l;
    EXPECT_EQ(ecuio_plugin_security_level(nullptr, 0, &l),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_security_level(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_security_level(p.get(), 2, &l),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_security_level(data_only.get(), 0, &l),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&l, &l_untouched, sizeof l), 0);
}

TEST(PluginConversationTest, AFoldersModuleIsReadCountedAndJudged)
{
    /* a folder's manifest takes no "files", so its module is a file */
    const Made m;
    {
        Folder f = folder_of(m);
        f.files.erase("guest.wasm");
        ecuio_status_t s = ECUIO_OK;
        Plugin p = load_folder(f, &s);
        EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
        const std::string why = ecuio_last_error_detail();
        EXPECT_NE(why.find("file 'guest.wasm' couldn't be read"),
                  std::string::npos)
            << why;
        EXPECT_EQ(why.compare(0, 20, "manifest.json, line "), 0) << why;
    }
    {
        Folder f = folder_of(m);
        f.files["guest.wasm"].assign(ecuio_wasm::MODULE_MAX_BYTES + 1, '\0');
        ecuio_status_t s = ECUIO_OK;
        Plugin p = load_folder(f, &s);
        EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("file 'guest.wasm' is over 1 MiB"),
                  std::string::npos)
            << ecuio_last_error_detail();
    }
    {
        Folder f = folder_of(m);
        f.files["guest.wasm"][8] ^= 0x01; /* one bit of the module */
        ecuio_status_t s = ECUIO_OK;
        Plugin p = load_folder(f, &s);
        EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("file 'guest.wasm' has SHA-256 "),
                  std::string::npos)
            << ecuio_last_error_detail();
    }
    {
        /* a module with a NUL and bytes past 0x7F is read as bytes */
        Made binary;
        ASSERT_NE(
            std::string(binary.module.begin(), binary.module.end()).find('\0'),
            std::string::npos);
        Folder f = folder_of(binary);
        Plugin p = load_folder(f);
        EXPECT_NE(p, nullptr) << ecuio_last_error_detail();
    }
    {
        /* the module's bytes count toward the package's 16 MiB: 16 data
         * files that with the manifest and the module come to it exactly,
         * then one byte more */
        Folder f = folder_of(m);
        std::string names;
        for (int i = 0; i < 16; ++i) {
            names += std::string(i > 0 ? ", " : "") + "\"d" +
                     std::to_string(i) + ".json\"";
        }
        std::string& manifest = f.files["manifest.json"];
        const size_t at = manifest.find("\"a.json\"");
        ASSERT_NE(at, std::string::npos);
        manifest.replace(at, std::strlen("\"a.json\""), names);
        const std::string data = conversation_test::kData;
        const auto padded = [&data](size_t size) {
            return data.substr(0, data.size() - 1) +
                   std::string(size - data.size(), ' ') + "}";
        };
        const size_t file = 1048576;
        for (int i = 0; i < 15; ++i) {
            f.files["d" + std::to_string(i) + ".json"] = padded(file);
        }
        const size_t last = file - manifest.size() - m.module.size();
        f.files["d15.json"] = padded(last);
        ecuio_status_t s = ECUIO_OK;
        Plugin whole = load_folder(f, &s);
        EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();

        f.files["d15.json"] = padded(last + 1);
        Plugin over = load_folder(f, &s);
        EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
        EXPECT_NE(std::string(ecuio_last_error_detail())
                      .find("the package is over 16 MiB"),
                  std::string::npos)
            << ecuio_last_error_detail();
    }
}

TEST(PluginConversationTest, APutAndADropRefuseAPluginWithTheTier)
{
    const Made m;
    const std::string packed = m.json();
    const std::string file =
        R"({"havne_signals": 1, "name": "Extra", "source": {"name": "t"}, )"
        R"("ecus": [{"id": "pcm", "rx": "7E8"}], "signals": [{"key": "oil2", )"
        R"("name": "Oil 2", "ecu": "pcm", "read": {"service": "22", )"
        R"("number": "F40E"}, "field": {"byte": 0}}]})";
    const char* const words =
        "the plugin has the conversation tier, which a put or a drop doesn't "
        "carry over";
    char* out = reinterpret_cast<char*>(1);
    size_t length = 7;
    EXPECT_EQ(ecuio_plugin_put_variant(packed.data(), packed.size(), "extra",
                                       file.data(), file.size(), &out, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(length, 0u);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find(words),
              std::string::npos)
        << ecuio_last_error_detail();
    out = reinterpret_cast<char*>(1);
    length = 7;
    EXPECT_EQ(ecuio_plugin_drop_variant(packed.data(), packed.size(), "base",
                                        &out, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(length, 0u);
    EXPECT_NE(std::string(ecuio_last_error_detail()).find(words),
              std::string::npos)
        << ecuio_last_error_detail();

    /* the same package with the data tier alone takes a put */
    Made plain;
    plain.tiers = R"(["data"])";
    plain.has_group = false;
    plain.has_binary = false;
    const std::string data_only = plain.json();
    out = nullptr;
    EXPECT_EQ(ecuio_plugin_put_variant(data_only.data(), data_only.size(),
                                       "extra", file.data(), file.size(), &out,
                                       &length),
              ECUIO_OK)
        << ecuio_last_error_detail();
    ecuio_plugin_text_free(out);
}
