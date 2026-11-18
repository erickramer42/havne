// File/layer: tests/test_plugin_conversation_rules.cpp | C++17, Qt-free,
//             every platform
// Testing: UNIT: PluginConversationTest (test_signals)
//
// The conversation tier's rules at load (ADR-0066, "havne_plugin" 2;
// core/src/plugin_conversation.cpp), each by a packed plugin that breaks
// one and the words of its refusal: the tier and its group, the declared
// services against tier 2's ceiling, the security levels, and the module
// by its file, hash and shape.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <string>

#include "wasm_check.h" /* core/src: MODULE_MAX_BYTES */

#include "plugin_conversation_support.h"

using conversation_test::base64;
using conversation_test::expect_refused;
using conversation_test::guest;
using conversation_test::Made;
using conversation_test::packed_refusal;
using conversation_test::sha_of;

TEST(PluginConversationTest, TheTiersAndTheGroupsRulesRefuseInTheirWords)
{
    expect_refused({
        /* the format, the tier and the key */
        {"format 3", [](Made& m) { m.format = "3"; },
         "\"havne_plugin\" is format 3, and this Havne reads formats 1 and 2"},
        {"the tier in format 1", [](Made& m) { m.format = "1"; },
         "tier \"conversation\" is format 2's: its manifest starts with "
         "\"havne_plugin\": 2"},
        {"a binary file in format 1",
         [](Made& m) {
             m.format = "1";
             m.tiers = R"(["data"])";
             m.has_group = false;
         },
         "file 'guest.wasm' is a binary file, which format 2 carries"},
        {"the group in format 1",
         [](Made& m) {
             m.format = "1";
             m.tiers = R"(["data"])";
             m.has_binary = false;
         },
         "\"conversation\" is format 2's"},
        {"the tier with no group",
         [](Made& m) {
             m.has_group = false;
             m.has_binary = false;
         },
         "the conversation tier needs \"conversation\""},
        {"the group with no tier", [](Made& m) { m.tiers = R"(["data"])"; },
         "\"conversation\" is the conversation tier's, which \"tiers\" "
         "doesn't list"},
        {"the tier alone", [](Made& m) { m.tiers = R"(["conversation"])"; },
         "\"tiers\" lists \"data\" too"},
        {"the tier twice",
         [](Made& m) {
             m.tiers = R"(["data", "conversation", "conversation"])";
         },
         "tier 'conversation' is listed twice"},
        {"a tier format 1 doesn't take",
         [](Made& m) {
             m.format = "1";
             m.tiers = R"(["data", "write"])";
             m.has_group = false;
             m.has_binary = false;
         },
         "tier 'write' isn't one format 1 takes: only \"data\""},
        {"a tier no Havne has",
         [](Made& m) { m.tiers = R"(["data", "write"])"; },
         "tier 'write' isn't one this Havne runs: \"data\" and "
         "\"conversation\""},
        {"a key the group doesn't take",
         [](Made& m) { m.group.extra = R"(, "writes": [])"; },
         "unknown key 'writes' in \"conversation\""},
        /* the budget */
        {"a budget of 0", [](Made& m) { m.group.budget = "0"; },
         "the conversation's budget"},
        {"a budget past the most", [](Made& m) { m.group.budget = "1000001"; },
         "the conversation's budget"},
        {"a budget with a fraction", [](Made& m) { m.group.budget = "10.5"; },
         "the conversation's budget"},
        /* the ECUs */
        {"no ecus", [](Made& m) { m.group.ecus.clear(); },
         "\"conversation\" needs \"ecus\""},
        {"an empty ecus", [](Made& m) { m.group.ecus = "[]"; },
         "the conversation's ecus lists 1 to 32 of the manifest's"},
        {"33 ecus, one more than a session's modules",
         [](Made& m) {
             conversation_test::all_32_ecus(m);
             m.group.ecus.insert(m.group.ecus.size() - 1, R"(, "pcm")");
         },
         "the conversation's ecus lists 1 to 32 of the manifest's"},
        {"an ECU the manifest lacks",
         [](Made& m) { m.group.ecus = R"(["pcm", "bcm"])"; },
         "the conversation's ecu 'bcm' isn't one \"ecus\" lists"},
        {"an ECU twice",
         [](Made& m) { m.group.ecus = R"(["pcm", "tcm", "pcm"])"; },
         "the conversation's ecus lists 'pcm' twice"},
    });
}

/* What a module may cause to be sent is inside tier 2's ceiling */
TEST(PluginConversationTest, EachServiceIsHeldToTheCeilingInItsWords)
{
    expect_refused({
        {"no services", [](Made& m) { m.group.services.clear(); },
         "\"conversation\" needs \"services\""},
        {"an empty services", [](Made& m) { m.group.services = "[]"; },
         "the conversation's services lists 1 to 16"},
        {"17 services",
         [](Made& m) {
             std::string list = "[";
             for (int i = 0; i < 17; ++i) {
                 list += std::string(i > 0 ? ", " : "") + R"({"sid": "22"})";
             }
             m.group.services = list + "]";
         },
         "the conversation's services lists 1 to 16"},
        {"a service with no sid",
         [](Made& m) { m.group.services = R"([{"sub": ["03"]}])"; },
         "a service needs \"sid\""},
        {"a key a service doesn't take",
         [](Made& m) {
             m.group.services = R"([{"sid": "22", "did": "F190"}])";
         },
         "unknown key 'did' in a service"},
        {"a sid of one digit",
         [](Made& m) { m.group.services = R"([{"sid": "2"}])"; },
         "a service's sid"},
        {"a sid in lower case",
         [](Made& m) { m.group.services = R"([{"sid": "2a"}])"; },
         "a service's sid '2a' takes two hex digits, 0-9 and A-F"},
        {"a service that writes",
         [](Made& m) { m.group.services = R"([{"sid": "2E"}])"; },
         "service 2E isn't one the conversation tier may send"},
        {"a routine", [](Made& m) { m.group.services = R"([{"sid": "31"}])"; },
         "service 31 isn't one the conversation tier may send"},
        {"a transfer", [](Made& m) { m.group.services = R"([{"sid": "34"}])"; },
         "service 34 isn't one the conversation tier may send"},
        {"a clear", [](Made& m) { m.group.services = R"([{"sid": "14"}])"; },
         "service 14 isn't one the conversation tier may send"},
        {"a reset", [](Made& m) { m.group.services = R"([{"sid": "11"}])"; },
         "service 11 isn't one the conversation tier may send"},
        {"a service twice",
         [](Made& m) {
             m.group.services =
                 R"([{"sid": "22"}, {"sid": "19"}, {"sid": "22"}])";
             m.group.security.clear();
         },
         "service 22 is given twice"},
        {"a split service with no sub",
         [](Made& m) { m.group.services = R"([{"sid": "10"}])"; },
         "service 10 is declared by sub-function: it needs \"sub\""},
        {"27 with no sub",
         [](Made& m) { m.group.services = R"([{"sid": "27"}])"; },
         "service 27 is declared by sub-function: it needs \"sub\""},
        {"a read with a sub",
         [](Made& m) {
             m.group.services = R"([{"sid": "22", "sub": ["F1"]}])";
         },
         "service 22 takes no \"sub\""},
        {"an empty sub",
         [](Made& m) { m.group.services = R"([{"sid": "10", "sub": []}])"; },
         "service 10's sub lists 1 to 14 sub-functions"},
        {"15 sub-functions",
         [](Made& m) {
             m.group.services =
                 R"([{"sid": "27", "sub": ["01", "02", "03", "04", "05", )"
                 R"("06", "07", "08", "09", "0A", "0B", "0C", "0D", "0E", )"
                 R"("0F"]}])";
         },
         "service 27's sub lists 1 to 14 sub-functions"},
        {"a sub-function twice",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["01", "02", "01"]}])";
         },
         "service 27 lists sub-function 01 twice"},
        {"a sub-function of one digit",
         [](Made& m) { m.group.services = R"([{"sid": "10", "sub": ["3"]}])"; },
         "service 10's sub-function"},
        {"the programming session",
         [](Made& m) {
             m.group.services = R"([{"sid": "10", "sub": ["02"]}])";
         },
         "service 10 02 isn't a request the conversation tier may send"},
        {"the default session",
         [](Made& m) {
             m.group.services = R"([{"sid": "10", "sub": ["01"]}])";
         },
         "service 10 01 isn't a request the conversation tier may send"},
        {"tester present with no reply",
         [](Made& m) {
             m.group.services = R"([{"sid": "3E", "sub": ["80"]}])";
         },
         "service 3E 80 isn't a request the conversation tier may send"},
        {"defining by memory address",
         [](Made& m) {
             m.group.services = R"([{"sid": "2C", "sub": ["01", "02"]}])";
         },
         "service 2C 02 isn't a request the conversation tier may send"},
        {"security access 00",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["00"]}])";
         },
         "service 27 00 isn't a request the conversation tier may send"},
        {"security access 5F",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["5F"]}])";
         },
         "service 27 5F isn't a request the conversation tier may send"},
        {"security access 60",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["60"]}])";
         },
         "service 27 60 isn't a request the conversation tier may send"},
        {"security access 7F",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["7F"]}])";
         },
         "service 27 7F isn't a request the conversation tier may send"},
        {"security access with bit 7",
         [](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["81"]}])";
         },
         "service 27 81 isn't a request the conversation tier may send"},
    });
}

TEST(PluginConversationTest, EachSecurityLevelsRuleRefusesInItsWords)
{
    const std::string level =
        R"({"ecu": "pcm", "level": "01", "seed_bytes": 4, "key_bytes": 4)";
    expect_refused({
        {"a key a level doesn't take",
         [level](Made& m) {
             m.group.security = "[" + level + R"(, "algorithm": "xor"}])";
         },
         "unknown key 'algorithm' in a security level"},
        {"a level with no ecu",
         [](Made& m) {
             m.group.security =
                 R"([{"level": "01", "seed_bytes": 4, "key_bytes": 4}])";
         },
         "a security level needs \"ecu\""},
        {"a level with no level",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "seed_bytes": 4, "key_bytes": 4}])";
         },
         "a security level needs \"level\""},
        {"a level on an ECU the group lacks",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "abs", "level": "01", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "a security level's ecu 'abs' isn't one the conversation's ecus "
         "lists"},
        {"an even level",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "02", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "security level 02 isn't a seed request's sub-function"},
        {"level 5F",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "5F", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "security level 5F isn't a seed request's sub-function"},
        {"level 7F",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "7F", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "security level 7F isn't a seed request's sub-function"},
        {"level FF",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "FF", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "security level FF isn't a seed request's sub-function"},
        {"a level twice for an ECU",
         [level](Made& m) {
             m.group.security = "[" + level + "}, " + level + "}]";
         },
         "security level 01 of ecu 'pcm' is given twice"},
        {"no seed length",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "key_bytes": 4}])";
         },
         "security level 01 needs \"seed_bytes\""},
        {"no key length",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4}])";
         },
         "security level 01 needs \"key_bytes\""},
        {"a seed of 0 bytes",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "seed_bytes": 0, )"
                 R"("key_bytes": 4}])";
         },
         "security level 01's seed_bytes"},
        {"a seed of 256 bytes",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "seed_bytes": 256, )"
                 R"("key_bytes": 4}])";
         },
         "security level 01's seed_bytes"},
        {"a key of 0 bytes",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, )"
                 R"("key_bytes": 0}])";
         },
         "security level 01's key_bytes"},
        {"a key of 59 bytes",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "01", "seed_bytes": 4, )"
                 R"("key_bytes": 59}])";
         },
         "security level 01's key_bytes"},
        {"a delay under a second",
         [level](Made& m) {
             m.group.security = "[" + level + R"(, "delay_ms": 999}])";
         },
         "security level 01's delay_ms"},
        {"a delay past an hour",
         [level](Made& m) {
             m.group.security = "[" + level + R"(, "delay_ms": 3600001}])";
         },
         "security level 01's delay_ms"},
        {"a key window of 49 ms",
         [level](Made& m) {
             m.group.security = "[" + level + R"(, "key_window_ms": 49}])";
         },
         "security level 01's key_window_ms"},
        {"a key window of 4001 ms",
         [level](Made& m) {
             m.group.security = "[" + level + R"(, "key_window_ms": 4001}])";
         },
         "security level 01's key_window_ms"},
        {"a level the services don't declare",
         [](Made& m) {
             m.group.security =
                 R"([{"ecu": "pcm", "level": "05", "seed_bytes": 4, )"
                 R"("key_bytes": 4}])";
         },
         "security level 05 needs service 27 declared with sub-functions 05 "
         "and 06"},
        {"a level whose key request isn't declared",
         [level](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["01", "03", "04"]}])";
             m.group.security = "[" + level + "}]";
         },
         "security level 01 needs service 27 declared with sub-functions 01 "
         "and 02"},
        {"a level whose seed request isn't declared",
         [level](Made& m) {
             m.group.services = R"([{"sid": "27", "sub": ["02"]}])";
             m.group.security = "[" + level + "}]";
         },
         "security level 01 needs service 27 declared with sub-functions 01 "
         "and 02"},
        {"a level with no 27 at all",
         [level](Made& m) {
             m.group.services = R"([{"sid": "22"}])";
             m.group.security = "[" + level + "}]";
         },
         "security level 01 needs service 27 declared"},
    });

    /* 33 levels: the list's own bound, before any level is read */
    Made many;
    std::string list = "[";
    for (int i = 0; i < 33; ++i) {
        list += std::string(i > 0 ? ", " : "") + level + "}";
    }
    many.group.security = list + "]";
    EXPECT_NE(packed_refusal(many).find(
                  "the conversation's security lists more than 32"),
              std::string::npos);
}

TEST(PluginConversationTest, TheCodeIsItsFilesBytesHashAndShape)
{
    const std::string zeros(64, '0');
    expect_refused({
        {"no code",
         [](Made& m) {
             m.has_code = false;
             m.has_binary = false;
         },
         "\"conversation\" needs \"code\""},
        {"code with no file",
         [zeros](Made& m) { m.group.code = "{\"sha256\": \"" + zeros + "\"}"; },
         "the conversation's code needs \"file\""},
        {"code with no hash",
         [](Made& m) { m.group.code = R"({"file": "guest.wasm"})"; },
         "the conversation's code needs \"sha256\""},
        {"a key the code doesn't take",
         [zeros](Made& m) {
             m.group.code = R"({"file": "guest.wasm", "sha256": ")" + zeros +
                            R"(", "size": 139})";
         },
         "unknown key 'size' in the conversation's code"},
        {"a hash in upper case", [](Made& m) { m.sha = std::string(64, 'A'); },
         "the code's sha256 takes 64 of 0-9 and a-f"},
        {"a hash of 63 digits", [](Made& m) { m.sha = std::string(63, 'a'); },
         "the code's sha256"},
        {"a path out of the package",
         [zeros](Made& m) {
             m.group.code =
                 R"({"file": "../guest.wasm", "sha256": ")" + zeros + "\"}";
         },
         "isn't a path in the package"},
        {"a file the package lacks", [](Made& m) { m.has_binary = false; },
         "file 'guest.wasm' isn't a binary file in \"files\""},
        {"a data file named as the code",
         [zeros](Made& m) {
             m.group.code = R"({"file": "a.json", "sha256": ")" + zeros + "\"}";
         },
         "file 'a.json' isn't a binary file in \"files\""},
        {"another module's hash", [zeros](Made& m) { m.sha = zeros; },
         "has SHA-256 "},
        {"a binary file of another kind",
         [](Made& m) { m.binary = R"({"kind": "elf", "base64": ""})"; },
         "a binary file's kind 'elf' isn't one this Havne reads: only "
         "\"wasm\""},
        {"a kind that is a number",
         [](Made& m) { m.binary = R"({"kind": 1, "base64": ""})"; },
         "a binary file's kind"},
        {"a key a binary file doesn't take",
         [](Made& m) {
             m.binary = R"({"kind": "wasm", "base64": "", "sha256": ""})";
         },
         "unknown key 'sha256' in a binary file"},
        {"a binary file with no bytes",
         [](Made& m) { m.binary = R"({"kind": "wasm"})"; },
         "a binary file needs \"base64\""},
        {"base64 that is a list",
         [](Made& m) { m.binary = R"({"kind": "wasm", "base64": []})"; },
         "a binary file's base64"},
        {"base64 with a space",
         [](Made& m) {
             std::string text = base64(m.module);
             text[8] = ' ';
             m.binary = R"({"kind": "wasm", "base64": ")" + text + "\"}";
         },
         "file 'guest.wasm''s base64 isn't RFC 4648's"},
        {"base64 with no padding",
         [](Made& m) {
             std::string text = base64(m.module);
             while (!text.empty() && text.back() == '=') {
                 text.pop_back();
             }
             m.binary = R"({"kind": "wasm", "base64": ")" + text + "\"}";
         },
         "file 'guest.wasm''s base64 isn't RFC 4648's"},
        {"a binary file as a variant's file",
         [](Made& m) {
             m.files_extra =
                 R"(, "spare.wasm": {"kind": "wasm", "base64": ""})";
             m.variant_files = R"(["a.json", "spare.wasm"])";
         },
         "file 'spare.wasm' isn't in \"files\""},
        {"a binary file in a plugin with no group",
         [](Made& m) {
             m.tiers = R"(["data"])";
             m.has_group = false;
         },
         "file 'guest.wasm' is a binary file nothing in the manifest names"},
        {"a second binary file nothing names",
         [](Made& m) {
             m.files_extra =
                 R"(, "spare.wasm": {"kind": "wasm", "base64": ""})";
         },
         "file 'spare.wasm' is a binary file nothing in the manifest names"},
        /* the module's shape, by wasm_check.h */
        {"bytes that are no module",
         [](Made& m) { m.module = {0x00, 0x61, 0x73}; },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"an empty file", [](Made& m) { m.module.clear(); },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"a module with no memory",
         [](Made& m) {
             wasm_builder::module g = guest();
             g.memory = false;
             m.module = g.build();
         },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"a module whose memory may grow",
         [](Made& m) {
             wasm_builder::module g = guest();
             g.memory_max = 2;
             m.module = g.build();
         },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"a module that imports",
         [](Made& m) {
             wasm_builder::module g = guest();
             g.imports.push_back({"env", "now"});
             m.module = g.build();
         },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"a module with a start function",
         [](Made& m) {
             wasm_builder::module g = guest();
             g.start = 5;
             m.module = g.build();
         },
         "file 'guest.wasm' isn't a module this Havne runs: "},
        {"a module past 1 MiB",
         [](Made& m) {
             m.module.assign(ecuio_wasm::MODULE_MAX_BYTES + 1, 0x00);
         },
         "file 'guest.wasm' is over 1 MiB"},
        {"1 MiB that is no module",
         [](Made& m) { m.module.assign(ecuio_wasm::MODULE_MAX_BYTES, 0x00); },
         "file 'guest.wasm' isn't a module this Havne runs: "},
    });

    /* The hash's refusal whole: the file, what it hashes to, what the
     * manifest says, at the hash's place */
    Made other;
    other.sha = zeros;
    const std::string why = packed_refusal(other);
    EXPECT_NE(why.find("file 'guest.wasm' has SHA-256 " + sha_of(other.module) +
                       ", and the manifest says " + zeros),
              std::string::npos)
        << why;
}

/* Each export of the guest ABI is needed, at its type */
TEST(PluginConversationTest, AModuleLackingAnExportOfTheGuestAbiIsRefused)
{
    for (size_t gone = 0; gone < 6; ++gone) {
        wasm_builder::module g = guest();
        const std::string name = g.funcs[gone].exported;
        g.funcs[gone].exported = "other";
        Made m;
        m.module = g.build();
        const std::string why = packed_refusal(m);
        EXPECT_NE(why.find("isn't a module this Havne runs: "),
                  std::string::npos)
            << name << ": " << why;
        EXPECT_NE(why.find(name), std::string::npos)
            << "the refusal names the export: " << why;
    }
    /* begin at another type: one parameter */
    wasm_builder::module g = guest();
    g.funcs[2].params = {wasm_builder::I32};
    Made m;
    m.module = g.build();
    const std::string why = packed_refusal(m);
    EXPECT_NE(why.find("isn't a module this Havne runs: "), std::string::npos)
        << why;
    EXPECT_NE(why.find("begin"), std::string::npos) << why;
}
