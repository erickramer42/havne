// File/layer: tests/test_plugin_operations.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginOperationTest (test_core)
//
// The plugin loader's reading of the operation tier (ADR-0066 section 4,
// "havne_plugin" 3; core/src/plugin_operations.cpp): a plan read back
// through plugin.h, packed and as a folder, what core derives from its
// steps, each bound's own value and each rule's refusal. Nothing here
// runs a plan.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/dump.h>
#include <ecuio/plugin.h>

#include <cstring>
#include <string>
#include <vector>

#include "plugin_conversation_support.h"

/* test_core.cpp's, over tests/new_counter.h */
void fail_new_on_this_thread(unsigned long n);

namespace {

using conversation_test::Folder;
using conversation_test::folder_of;
using conversation_test::load_folder;
using conversation_test::load_packed;
using conversation_test::Made;
using conversation_test::packed_refusal;
using conversation_test::Plugin;

const char kPublisher[] =
    R"({"name": "Made-up Tools", "contact": "c", "license": "MIT", )"
    R"("terms": "t"})";

const char kRead[] =
    R"({"step": "read", "name": "calibration", "session": "extended", )"
    R"("address": "00010000", "bytes": 65536, "address_bytes": 4, )"
    R"("size_bytes": 2, "chunk": 1024})";
const char kUpload[] =
    R"({"step": "upload", "name": "boot", "session": "extended", )"
    R"("address": "00020000", "bytes": 4096, "address_bytes": 4, )"
    R"("size_bytes": 4, "dfi": "00"})";
const char kIdent[] =
    R"([{"did": "D220", "role": "hardware"}, {"did": "D221", "role": )"
    R"("software"}])";

/* One operation's JSON: each part a case's to change */
struct Op {
    std::string id = "\"dump-calibration\"";
    std::string kind = "\"dump\"";
    std::string title = "\"Calibration\"";
    std::string ecu = "\"pcm\"";
    std::string level = "\"01\"";
    std::string ident = kIdent;
    std::string steps = std::string("[") + kRead + ", " + kUpload + "]";
    std::string extra; /* more keys, each after a comma */

    std::string json() const
    {
        std::string out;
        const auto key = [&out](const char* name, const std::string& text) {
            if (!text.empty()) {
                out += std::string(out.empty() ? "{\"" : ", \"") + name +
                       "\": " + text;
            }
        };
        key("id", id);
        key("kind", kind);
        key("title", title);
        key("ecu", ecu);
        key("level", level);
        key("ident", ident);
        key("steps", steps);
        return (out.empty() ? "{" : out) + extra + "}";
    }
};

/* A format-3 plugin with the three tiers and these operations */
Made made(const std::string& operations)
{
    Made m;
    m.format = "3";
    m.tiers = R"(["data", "conversation", "operation"])";
    m.publisher = kPublisher;
    m.operations = operations;
    return m;
}

Made made(const Op& op = Op())
{
    return made("[" + op.json() + "]");
}

/* The refusal's words after their place */
std::string why(const Made& m)
{
    const std::string words = packed_refusal(m);
    const size_t at = words.find(": ");
    return at == std::string::npos ? words : words.substr(at + 2);
}

std::string why(const Op& op)
{
    return why(made(op));
}

std::string step_why(const std::string& step)
{
    Op op;
    op.steps = "[" + step + "]";
    return why(op);
}

/* kRead with one key's value replaced, or the key removed ("") */
std::string read_with(const std::string& key, const std::string& value)
{
    std::string s = kRead;
    const std::string find = "\"" + key + "\": ";
    const size_t at = s.find(find);
    EXPECT_NE(at, std::string::npos) << key;
    size_t end = s.find(", \"", at);
    if (end == std::string::npos) {
        end = s.size() - 1;
    }
    if (value.empty()) {
        const bool last = end == s.size() - 1;
        s.erase(last ? at - 2 : at, last ? end - at + 2 : end - at + 2);
    } else {
        s.replace(at + find.size(), end - at - find.size(), value);
    }
    return s;
}

ecuio_plugin_operation_t operation_of(const ecuio_plugin_t* p, uint32_t index)
{
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    EXPECT_EQ(ecuio_plugin_operation(p, index, &o), ECUIO_OK);
    return o;
}

ecuio_plugin_operation_step_t step_of(const ecuio_plugin_t* p, uint32_t op,
                                      uint32_t index)
{
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    EXPECT_EQ(ecuio_plugin_operation_step(p, op, index, &s), ECUIO_OK);
    return s;
}

void expect_the_default(const ecuio_plugin_t* p)
{
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_plugin_info(p, &info), ECUIO_OK);
    EXPECT_EQ(info.tiers, ECUIO_PLUGIN_TIER_DATA |
                              ECUIO_PLUGIN_TIER_CONVERSATION |
                              ECUIO_PLUGIN_TIER_OPERATION);
    uint32_t count = 99;
    ASSERT_EQ(ecuio_plugin_operation_count(p, &count), ECUIO_OK);
    ASSERT_EQ(count, 1u);

    const ecuio_plugin_operation_t o = operation_of(p, 0);
    EXPECT_EQ(o.struct_size, sizeof o);
    EXPECT_EQ(o.flags, 0u);
    EXPECT_EQ(o.kind, ECUIO_OPERATION_KIND_DUMP);
    EXPECT_EQ(o.op_class, ECUIO_OPERATION_CLASS_READ);
    EXPECT_EQ(o.rx_id, 0x7E8u);
    EXPECT_EQ(o.tx_id, 0x7E0u);
    EXPECT_EQ(o.level, 0x01u);
    EXPECT_EQ(o.step_count, 2u);
    EXPECT_EQ(o.ident_count, 2u);
    EXPECT_EQ(o.unlock_sessions, 1u);
    EXPECT_EQ(o.image_bytes, 65536u + 4096u);
    EXPECT_STREQ(o.id, "dump-calibration");
    EXPECT_STREQ(o.title, "Calibration");
    EXPECT_EQ(o.leave, 0u) << "an extended plan's session stays";
    for (const uint32_t r : o.reserved) {
        EXPECT_EQ(r, 0u);
    }

    const ecuio_plugin_operation_step_t read = step_of(p, 0, 0);
    EXPECT_EQ(read.struct_size, sizeof read);
    EXPECT_EQ(read.kind, ECUIO_STEP_READ);
    EXPECT_EQ(read.session, ECUIO_STEP_SESSION_EXTENDED);
    EXPECT_EQ(read.address, 0x00010000u);
    EXPECT_EQ(read.bytes, 65536u);
    EXPECT_EQ(read.chunk, 1024u);
    EXPECT_EQ(read.address_bytes, 4);
    EXPECT_EQ(read.size_bytes, 2);
    EXPECT_EQ(read.dfi, 0);
    EXPECT_STREQ(read.name, "calibration");
    const ecuio_plugin_operation_step_t upload = step_of(p, 0, 1);
    EXPECT_EQ(upload.kind, ECUIO_STEP_UPLOAD);
    EXPECT_EQ(upload.address, 0x00020000u);
    EXPECT_EQ(upload.bytes, 4096u);
    EXPECT_EQ(upload.chunk, 0u) << "the module names an upload's block";
    EXPECT_EQ(upload.size_bytes, 4);
    EXPECT_STREQ(upload.name, "boot");

    ecuio_plugin_operation_ident_t d{};
    d.struct_size = sizeof d;
    ASSERT_EQ(ecuio_plugin_operation_ident(p, 0, 0, &d), ECUIO_OK);
    EXPECT_EQ(d.did, 0xD220u);
    EXPECT_EQ(d.role, ECUIO_DUMP_ROLE_HARDWARE);
    ASSERT_EQ(ecuio_plugin_operation_ident(p, 0, 1, &d), ECUIO_OK);
    EXPECT_EQ(d.did, 0xD221u);
    EXPECT_EQ(d.role, ECUIO_DUMP_ROLE_SOFTWARE);
}

TEST(PluginOperationTest, APlanReadsBackPackedAndFromAFolder)
{
    const Made m = made();
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    const Plugin packed = load_packed(m.json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_default(packed.get());
    Folder f = folder_of(m);
    const Plugin folder = load_folder(f, &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_default(folder.get());
}

TEST(PluginOperationTest, TheGettersCheckTheirArguments)
{
    const Plugin p = load_packed(made().json());
    ASSERT_NE(p, nullptr);
    ecuio_plugin_operation_t o;
    std::memset(&o, 0xA5, sizeof o);
    o.struct_size = sizeof o;
    ecuio_plugin_operation_step_t s;
    std::memset(&s, 0xA5, sizeof s);
    s.struct_size = sizeof s;
    ecuio_plugin_operation_ident_t d;
    std::memset(&d, 0xA5, sizeof d);
    d.struct_size = sizeof d;
    uint32_t count = 0;
    EXPECT_EQ(ecuio_plugin_operation_count(nullptr, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_count(p.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation(nullptr, 0, &o), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step(nullptr, 0, 0, &s),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step(p.get(), 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_ident(nullptr, 0, 0, &d),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_ident(p.get(), 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    /* At or past each count, and a struct too small: out untouched */
    EXPECT_EQ(ecuio_plugin_operation(p.get(), 1, &o),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step(p.get(), 1, 0, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step(p.get(), 0, 2, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_ident(p.get(), 1, 0, &d),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_ident(p.get(), 0, 2, &d),
              ECUIO_ERR_INVALID_ARGUMENT);
    o.struct_size = sizeof o - 1;
    s.struct_size = sizeof s - 1;
    d.struct_size = sizeof d - 1;
    EXPECT_EQ(ecuio_plugin_operation(p.get(), 0, &o),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step(p.get(), 0, 0, &s),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_ident(p.get(), 0, 0, &d),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(o.kind, 0xA5A5A5A5u);
    EXPECT_EQ(s.kind, 0xA5A5A5A5u);
    EXPECT_EQ(d.did, 0xA5A5A5A5u);

    /* A plugin without the tier has no operation */
    const Plugin plain = load_packed(Made().json());
    ASSERT_NE(plain, nullptr);
    count = 99;
    EXPECT_EQ(ecuio_plugin_operation_count(plain.get(), &count), ECUIO_OK);
    EXPECT_EQ(count, 0u);
    o.struct_size = sizeof o;
    EXPECT_EQ(ecuio_plugin_operation(plain.get(), 0, &o),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(PluginOperationTest, TheTierItsGroupAndItsFormatGoTogether)
{
    Made m = made();
    m.operations.clear();
    EXPECT_EQ(why(m), "the operation tier needs \"operations\": its plans");
    m = made();
    m.tiers = R"(["data", "conversation"])";
    EXPECT_EQ(why(m), "\"operations\" is the operation tier's, which "
                      "\"tiers\" doesn't list");
    m = made();
    m.format = "2";
    m.publisher.clear();
    EXPECT_EQ(why(m), "tier \"operation\" is format 3's: its manifest starts "
                      "with \"havne_plugin\": 3");
    m.tiers = R"(["data", "conversation"])";
    EXPECT_EQ(why(m), "\"operations\" is format 3's: its manifest starts with "
                      "\"havne_plugin\": 3");
    /* Without the conversation tier a plan has no ECU and no unlock */
    m = made();
    m.tiers = R"(["data", "operation"])";
    m.has_group = false;
    m.has_binary = false;
    m.publisher.clear();
    EXPECT_EQ(why(m), "the operation tier needs the conversation tier: a "
                      "plan names one of its ECUs and its unlock");
    m = made("{}");
    EXPECT_EQ(why(m), "\"operations\" is an object, not a list");
    m = made("[]");
    EXPECT_EQ(why(m), "\"operations\" lists 1 to 8");
}

TEST(PluginOperationTest, EachCountsBoundIsHeld)
{
    /* 8 operations, and one more */
    std::string list;
    for (int i = 0; i < 9; ++i) {
        Op op;
        op.id = "\"dump-" + std::to_string(i) + "\"";
        if (i == 8) {
            EXPECT_NE(load_packed(made("[" + list + "]").json()), nullptr)
                << ecuio_last_error_detail();
        }
        list += (i == 0 ? "" : ", ") + op.json();
    }
    EXPECT_EQ(why(made("[" + list + "]")), "\"operations\" lists 1 to 8");

    /* 64 steps, and one more */
    std::string steps;
    for (int i = 0; i < 65; ++i) {
        char address[16];
        std::snprintf(address, sizeof address, "\"%08X\"", 0x10000 + 16 * i);
        std::string s = read_with("address", address);
        s = s.replace(s.find("calibration"), 11, "r" + std::to_string(i));
        s = s.replace(s.find("65536"), 5, "16");
        if (i == 64) {
            Op op;
            op.steps = "[" + steps + "]";
            const Plugin p = load_packed(made(op).json());
            ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
            EXPECT_EQ(operation_of(p.get(), 0).step_count, 64u);
            EXPECT_EQ(operation_of(p.get(), 0).image_bytes, 64u * 16u);
        }
        steps += (i == 0 ? "" : ", ") + s;
    }
    Op op;
    op.steps = "[" + steps + "]";
    EXPECT_EQ(why(op), "an operation's steps lists 1 to 64");
    op.steps = "[]";
    EXPECT_EQ(why(op), "an operation's steps lists 1 to 64");

    /* 8 identification DIDs, and one more */
    std::string ident;
    for (int i = 0; i < 9; ++i) {
        if (i == 8) {
            Op with;
            with.ident = "[" + ident + "]";
            const Plugin p = load_packed(made(with).json());
            ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
            EXPECT_EQ(operation_of(p.get(), 0).ident_count, 8u);
        }
        ident += std::string(i == 0 ? "" : ", ") + "{\"did\": \"D22" +
                 std::to_string(i) + "\", \"role\": \"base\"}";
    }
    op = Op();
    op.ident = "[" + ident + "]";
    EXPECT_EQ(why(op), "an operation's ident lists more than 8");
    /* None at all is a plan too */
    op.ident.clear();
    const Plugin none = load_packed(made(op).json());
    ASSERT_NE(none, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(operation_of(none.get(), 0).ident_count, 0u);
}

TEST(PluginOperationTest, EachOperationRuleRefusesInItsWords)
{
    Op op;
    op.extra = ", \"class\": \"read\"";
    EXPECT_EQ(why(op), "unknown key 'class' in an operation")
        << "the class is derived, never declared";
    op = Op();
    op.id = "\"Dump\"";
    EXPECT_EQ(why(op), "an operation's id 'Dump' takes a-z, 0-9 and -, a "
                       "letter or digit at each end");
    op = Op();
    op.kind = "\"flash\"";
    EXPECT_EQ(why(op), "operation 'dump-calibration''s kind 'flash' isn't "
                       "one this Havne runs: \"dump\" and \"kernel_read\"");
    op = Op();
    op.title = "\"\"";
    EXPECT_NE(why(op).find("title"), std::string::npos);
    op = Op();
    op.ecu = "\"abs\"";
    EXPECT_EQ(why(op), "operation 'dump-calibration''s ecu 'abs' isn't one "
                       "the conversation's ecus lists");
    op = Op();
    op.level = "\"03\"";
    EXPECT_EQ(why(op), "operation 'dump-calibration''s level isn't one the "
                       "conversation's security declares for ecu 'pcm'")
        << "03 is the tcm's";
    op = Op();
    op.level.clear();
    EXPECT_EQ(why(op), "operation 'dump-calibration' needs 'level'");
    for (const char* key : {"id", "kind", "title", "ecu", "steps"}) {
        op = Op();
        (std::string(key) == "id"      ? op.id
         : std::string(key) == "kind"  ? op.kind
         : std::string(key) == "title" ? op.title
         : std::string(key) == "ecu"   ? op.ecu
                                       : op.steps)
            .clear();
        EXPECT_NE(why(op).find(std::string("needs '") + key + "'"),
                  std::string::npos)
            << key << ": " << why(op);
    }
    /* Two of one id */
    EXPECT_EQ(why(made("[" + Op().json() + ", " + Op().json() + "]")),
              "operation 'dump-calibration' is given twice");

    /* The identification */
    op = Op();
    op.ident = R"([{"did": "D220", "role": "serial"}])";
    EXPECT_EQ(why(op), "an identification's role 'serial' isn't one of "
                       "\"hardware\", \"software\" and \"base\"");
    op.ident = R"([{"did": "d220", "role": "base"}])";
    EXPECT_EQ(why(op), "an identification's did 'd220' takes pairs of hex "
                       "digits, 0-9 and A-F");
    op.ident = R"([{"did": "D220", "role": "base"}, {"did": "D220", )"
               R"("role": "hardware"}])";
    EXPECT_EQ(why(op), "an identification's did is given twice");
    Made m = made();
    m.group.services = R"([{"sid": "10", "sub": ["03"]}, )"
                       R"({"sid": "27", "sub": ["01", "02", "03", "04"]}, )"
                       R"({"sid": "3E", "sub": ["00"]}])";
    EXPECT_EQ(why(m), "an operation's ident needs service 22 among the "
                      "conversation's services");
}

TEST(PluginOperationTest, EachStepRuleRefusesInItsWords)
{
    /* A kind a later Havne runs is refused by its name */
    for (const char* kind : {"reset", "write"}) {
        EXPECT_EQ(step_why(read_with("step", std::string("\"") + kind + "\"")),
                  std::string("step '") + kind +
                      "' isn't one this Havne runs: \"read\" and \"upload\"");
    }
    for (const char* kind : {"download", "routine"}) {
        EXPECT_EQ(step_why(read_with("step", std::string("\"") + kind + "\"")),
                  "step 'calibration': a kernel read's download and its "
                  "routine are its first two steps, and only a kernel read's");
    }
    EXPECT_EQ(step_why(read_with("session", "\"safety\"")),
              "step 'calibration''s session 'safety' isn't one this Havne "
              "reads in: \"extended\" and \"programming\"");
    EXPECT_EQ(step_why(read_with("name", "\"Cal\"")),
              "a step's name 'Cal' takes a-z, 0-9 and -, a letter or digit "
              "at each end");
    EXPECT_EQ(step_why(read_with("address", "\"0001000\"")),
              "step 'calibration''s address '0001000' takes pairs of hex "
              "digits, 0-9 and A-F");
    EXPECT_NE(step_why(read_with("bytes", "0")).find("bytes"),
              std::string::npos);
    EXPECT_NE(step_why(read_with("address_bytes", "5")).find("address_bytes"),
              std::string::npos);
    EXPECT_NE(step_why(read_with("size_bytes", "0")).find("size_bytes"),
              std::string::npos);
    EXPECT_NE(step_why(read_with("chunk", "4095")).find("chunk"),
              std::string::npos);
    EXPECT_EQ(step_why(read_with("chunk", "")),
              "step 'calibration' needs 'chunk'");
    for (const char* key : {"step", "name", "session", "address", "bytes",
                            "address_bytes", "size_bytes"}) {
        EXPECT_NE(step_why(read_with(key, ""))
                      .find(std::string("needs '") + key + "'"),
                  std::string::npos)
            << key << ": " << step_why(read_with(key, ""));
    }
    /* Each number fits the bytes that carry it */
    EXPECT_EQ(step_why(read_with("address_bytes", "2")),
              "step 'calibration': its last address doesn't fit its "
              "address_bytes");
    EXPECT_EQ(step_why(read_with("size_bytes", "1")),
              "step 'calibration': its chunk doesn't fit its size_bytes");
    EXPECT_EQ(step_why(read_with("address", "\"FFFFFF00\"")),
              "step 'calibration': its last address doesn't fit its "
              "address_bytes")
        << "past the 32-bit addresses";
    /* A read takes no dfi, an upload no chunk and no dfi but 00 */
    std::string s = kRead;
    s.insert(s.size() - 1, ", \"dfi\": \"00\"");
    EXPECT_EQ(step_why(s), "step 'calibration' is a read, which takes no "
                           "\"dfi\"");
    s = kUpload;
    s.insert(s.size() - 1, ", \"chunk\": 512");
    EXPECT_EQ(step_why(s), "step 'boot' is an upload, which takes no "
                           "\"chunk\": the module names its block");
    for (const char* dfi : {"\"01\"", "\"10\"", "\"11\""}) {
        s = kUpload;
        s.replace(s.find("\"00\""), 4, dfi);
        EXPECT_EQ(step_why(s), "step 'boot''s dfi is \"00\": a transformed "
                               "upload isn't the memory a copy holds")
            << dfi;
    }
    s = kUpload;
    s.replace(s.find("\"size_bytes\": 4"), 15, "\"size_bytes\": 1");
    EXPECT_EQ(step_why(s), "step 'boot': its bytes doesn't fit its size_bytes");
    s.insert(s.size() - 1, ", \"length\": 1");
    EXPECT_EQ(step_why(s), "unknown key 'length' in a step");

    /* Two steps of one name, and two that share an address */
    Op op;
    op.steps = std::string("[") + kRead + ", " + kRead + "]";
    EXPECT_EQ(why(op), "step 'calibration' is given twice");
    std::string second = read_with("address", "\"0001FFFF\"");
    second.replace(second.find("calibration"), 11, "tail");
    op.steps = std::string("[") + kRead + ", " + second + "]";
    EXPECT_EQ(why(op), "steps 'calibration' and 'tail' overlap in address");
    second = read_with("address", "\"00020000\"");
    second.replace(second.find("calibration"), 11, "tail");
    op.steps = std::string("[") + kRead + ", " + second + "]";
    EXPECT_NE(load_packed(made(op).json()), nullptr)
        << "end to start is no overlap: " << ecuio_last_error_detail();
}

/* The steps' bytes add to at most what a copy holds */
TEST(PluginOperationTest, AProgrammingPlanNamesHowItsSessionIsLeft)
{
    const auto programming = [](std::string step) {
        const std::string from = "\"session\": \"extended\"";
        step.replace(step.find(from), from.size(),
                     "\"session\": \"programming\"");
        return step;
    };
    Op op;
    op.steps = "[" + programming(kRead) + ", " + programming(kUpload) + "]";
    const auto loaded = [](const Op& o) {
        ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
        const Plugin p = load_packed(made(o).json(), &s);
        EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
        return p.get() == nullptr ? ecuio_plugin_operation_t{}
                                  : operation_of(p.get(), 0);
    };
    ecuio_plugin_operation_t o = loaded(op);
    EXPECT_EQ(o.leave, ECUIO_OPERATION_LEAVE_DEFAULT) << "10 01 when unnamed";
    EXPECT_EQ(o.unlock_sessions, 1u);
    op.extra = R"(, "leave": "reset")";
    o = loaded(op);
    EXPECT_EQ(o.leave, ECUIO_OPERATION_LEAVE_RESET);

    op.extra = R"(, "leave": "exit")";
    EXPECT_EQ(why(op), "operation 'dump-calibration''s leave 'exit' isn't one "
                       "of \"default\" (10 01) and \"reset\" (11 01)");

    op.steps = "[" + programming(kRead) + ", " + kUpload + "]";
    op.extra.clear();
    EXPECT_EQ(why(op), "steps 'calibration' and 'boot' are in two sessions: "
                       "this Havne reads a plan's steps in one");
    Op extended;
    extended.extra = R"(, "leave": "default")";
    EXPECT_EQ(why(extended), "operation 'dump-calibration''s steps are in the "
                             "extended session, which stays after the run: "
                             "it takes no 'leave'");
}

TEST(PluginOperationTest, APlansImageFitsACopy)
{
    const auto step = [](const char* name, const char* address,
                         uint64_t bytes) {
        return std::string(R"({"step": "upload", "name": ")") + name +
               R"(", "session": "extended", "address": ")" + address +
               R"(", "bytes": )" + std::to_string(bytes) +
               R"(, "address_bytes": 4, "size_bytes": 4})";
    };
    Op op;
    op.steps = "[" + step("a", "00000000", ECUIO_DUMP_IMAGE_MAX - 16) + ", " +
               step("b", "10000000", 16) + "]";
    const Plugin p = load_packed(made(op).json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    EXPECT_EQ(operation_of(p.get(), 0).image_bytes, ECUIO_DUMP_IMAGE_MAX);
    op.steps = "[" + step("a", "00000000", ECUIO_DUMP_IMAGE_MAX - 16) + ", " +
               step("b", "10000000", 17) + "]";
    EXPECT_EQ(why(op), "an operation's steps add to more than a copy holds: "
                       "67108864 bytes");
    op.steps = "[" + step("a", "00000000", ECUIO_DUMP_IMAGE_MAX + 1) + "]";
    EXPECT_NE(why(op).find("bytes"), std::string::npos);
}

TEST(PluginOperationTest, APutAndADropRefuseAPluginWithTheTier)
{
    const std::string packed = made().json();
    const std::string file = conversation_test::kData;
    char* out = nullptr;
    size_t length = 0;
    EXPECT_EQ(ecuio_plugin_put_variant(packed.data(), packed.size(), "extra",
                                       file.data(), file.size(), &out, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(ecuio_plugin_drop_variant(packed.data(), packed.size(), "base",
                                        &out, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out, nullptr);
}

/* Each allocation of a clean load fails in turn: OK or OUT_OF_MEMORY,
 * nothing thrown, and an OK is whole */
TEST(PluginOperationTest, ALoadSurvivesEveryAllocationFailing)
{
    const std::string packed = made().json();
    unsigned long refused = 0;
    for (unsigned long n = 1;; ++n) {
        ecuio_plugin_t* p = nullptr;
        fail_new_on_this_thread(n);
        const ecuio_status_t s = ecuio_plugin_load(packed.data(), packed.size(),
                                                   nullptr, nullptr, &p);
        fail_new_on_this_thread(0);
        ASSERT_TRUE(s == ECUIO_OK || s == ECUIO_ERR_OUT_OF_MEMORY)
            << n << ": " << s << " " << ecuio_last_error_detail();
        if (s == ECUIO_OK) {
            expect_the_default(p);
            EXPECT_EQ(ecuio_plugin_free(p), ECUIO_OK);
            break;
        }
        ASSERT_EQ(p, nullptr) << n;
        ++refused;
        ASSERT_LT(n, 20000u) << "the loop should end";
    }
    EXPECT_GT(refused, 50u) << "a load allocates";
}

} // namespace
