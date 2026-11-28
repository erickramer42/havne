#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstring>
#include <string>
#include <vector>

#include "plugin_kernel_support.h"

#include "../core/src/operation_plan.h"

namespace {

using conversation_test::base64;
using conversation_test::bytes;
using conversation_test::Folder;
using conversation_test::folder_of;
using conversation_test::load_folder;
using conversation_test::load_packed;
using conversation_test::Made;
using conversation_test::packed_refusal;
using conversation_test::Plugin;
using conversation_test::sha_of;

using kernel_test::helper;
using kernel_test::Kernel;
using kernel_test::kPublisher;
using kernel_test::made;

std::string why(const Made& m)
{
    const std::string words = packed_refusal(m);
    const size_t at = words.find(": ");
    return at == std::string::npos ? words : words.substr(at + 2);
}

std::string why(const Kernel& k)
{
    return why(made(k));
}

void expect_the_kernel(const ecuio_plugin_t* p, const bytes& blob)
{
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    ASSERT_EQ(ecuio_plugin_operation(p, 0, &o), ECUIO_OK);
    EXPECT_EQ(o.kind, ECUIO_OPERATION_KIND_KERNEL_READ);
    EXPECT_EQ(o.op_class, ECUIO_OPERATION_CLASS_WRITE)
        << "a 34 and a 31 are no reads";
    EXPECT_EQ(o.step_count, 3u);
    EXPECT_EQ(o.image_bytes, 4096u) << "the copy holds the ranges alone";

    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    ASSERT_EQ(ecuio_plugin_operation_step(p, 0, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.kind, ECUIO_STEP_DOWNLOAD);
    EXPECT_EQ(s.session, ECUIO_STEP_SESSION_PROGRAMMING);
    EXPECT_EQ(s.address, 0x00F00000u);
    EXPECT_EQ(s.bytes, blob.size());
    EXPECT_EQ(s.address_bytes, 4);
    EXPECT_EQ(s.size_bytes, 4);
    EXPECT_EQ(s.routine_id, 0u);
    EXPECT_STREQ(s.name, "helper");
    ASSERT_EQ(ecuio_plugin_operation_step(p, 0, 1, &s), ECUIO_OK);
    EXPECT_EQ(s.kind, ECUIO_STEP_ROUTINE);
    EXPECT_EQ(s.routine_id, 0x0201u);
    EXPECT_EQ(s.bytes, 0u);
    EXPECT_STREQ(s.name, "start");
    ASSERT_EQ(ecuio_plugin_operation_step(p, 0, 2, &s), ECUIO_OK);
    EXPECT_EQ(s.kind, ECUIO_STEP_UPLOAD);
    EXPECT_EQ(s.address, 0x00080000u);
    EXPECT_STREQ(s.name, "code");
    for (const uint32_t r : s.reserved) {
        EXPECT_EQ(r, 0u);
    }
    EXPECT_EQ(ecuio_plugin_operation_step(p, 0, 3, &s),
              ECUIO_ERR_INVALID_ARGUMENT);

    ecuio_plugin_operation_kernel_t k{};
    k.struct_size = sizeof k;
    ASSERT_EQ(ecuio_plugin_operation_kernel(p, 0, &k), ECUIO_OK);
    EXPECT_EQ(k.struct_size, sizeof k);
    EXPECT_EQ(k.flags, 0u);
    EXPECT_EQ(k.ram_address, 0x00F00000u);
    EXPECT_EQ(k.ram_bytes, 16384u);
    ASSERT_EQ(k.start_count, 1u);
    EXPECT_EQ(k.start[0], 0x0201);
    EXPECT_EQ(k.start[1], 0);
    EXPECT_EQ(k.quiet_session, 0x82);
    EXPECT_EQ(k.quiet_keepalive, 0x80);
    EXPECT_EQ(k.keepalive_ms, 2000u);
    EXPECT_EQ(k.stays, 1u);
    EXPECT_EQ(k.reserved0, 0);
    for (const uint32_t r : k.reserved) {
        EXPECT_EQ(r, 0u);
    }

    const uint8_t* got = nullptr;
    uint64_t length = 0;
    ASSERT_EQ(ecuio_plugin_operation_blob(p, 0, &got, &length), ECUIO_OK);
    ASSERT_EQ(length, blob.size());
    EXPECT_EQ(std::memcmp(got, blob.data(), blob.size()), 0);
}

} // namespace

TEST(PluginOperationTest, AKernelReadReadsBackPackedAndFromAFolder)
{
    const Kernel k;
    const Made m = made(k);
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    const Plugin packed = load_packed(m.json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_kernel(packed.get(), k.blob);
    Folder f = folder_of(m);
    f.files["kernel.bin"] = std::string(k.blob.begin(), k.blob.end());
    const Plugin folder = load_folder(f, &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_kernel(folder.get(), k.blob);

    ecuio_plugin_fingerprint_t a{};
    a.struct_size = sizeof a;
    ecuio_plugin_fingerprint_t b{};
    b.struct_size = sizeof b;
    ASSERT_EQ(ecuio_plugin_fingerprint(packed.get(), &a), ECUIO_OK);
    Kernel other;
    other.blob[7] ^= 0x01;
    const Plugin changed = load_packed(made(other).json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_plugin_fingerprint(changed.get(), &b), ECUIO_OK);
    EXPECT_NE(std::memcmp(a.fingerprint, b.fingerprint, sizeof a.fingerprint),
              0)
        << "a consent covers the helper's bytes";
}

TEST(PluginOperationTest, TheKernelGettersCheckTheirArguments)
{
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    const Plugin p = load_packed(made(Kernel()).json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_plugin_operation_kernel_t k{};
    k.struct_size = sizeof k;
    EXPECT_EQ(ecuio_plugin_operation_kernel(nullptr, 0, &k),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_kernel(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_kernel(p.get(), 1, &k),
              ECUIO_ERR_INVALID_ARGUMENT);
    k.struct_size = sizeof k - 1;
    EXPECT_EQ(ecuio_plugin_operation_kernel(p.get(), 0, &k),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(k.ram_bytes, 0u) << "out untouched";
    const uint8_t* got = nullptr;
    uint64_t length = 7;
    EXPECT_EQ(ecuio_plugin_operation_blob(p.get(), 0, nullptr, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_blob(p.get(), 0, &got, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_blob(p.get(), 1, &got, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(length, 7u);

    Made dump;
    dump.format = "3";
    dump.tiers = R"(["data", "conversation", "operation"])";
    dump.publisher = kPublisher;
    dump.operations =
        R"([{"id": "copy", "kind": "dump", "title": "Copy", "ecu": "pcm", )"
        R"("level": "01", "steps": [{"step": "upload", "name": "code", )"
        R"("session": "extended", "address": "00080000", "bytes": 4096, )"
        R"("address_bytes": 4, "size_bytes": 4}]}])";
    const Plugin d = load_packed(dump.json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    k.struct_size = sizeof k;
    EXPECT_EQ(ecuio_plugin_operation_kernel(d.get(), 0, &k),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a dump has no helper";
    EXPECT_EQ(ecuio_plugin_operation_blob(d.get(), 0, &got, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(PluginOperationTest, AKernelReadsRefusalsSayWhy)
{
    Kernel k;
    k.download_address = "\"00F03F00\"";
    EXPECT_EQ(why(k), "operation 'kernel-read': its download's address and "
                      "length fall outside its RAM window")
        << "the address and the length, not the address alone";
    k = Kernel();
    k.download_address = "\"00EFFFFF\"";
    EXPECT_EQ(why(k), "operation 'kernel-read': its download's address and "
                      "length fall outside its RAM window");
    k = Kernel();
    k.blob = helper(16385);
    k.download_address = "\"00F00000\"";
    EXPECT_EQ(why(k), "operation 'kernel-read': its download's address and "
                      "length fall outside its RAM window")
        << "a helper longer than its window";
    k = Kernel();
    k.ram = R"({"address": "00080800", "bytes": 16384})";
    k.download_address = "\"00080800\"";
    EXPECT_EQ(why(k), "operation 'kernel-read': its RAM window shares an "
                      "address with a range it reads");
    k = Kernel();
    k.routine = "\"FF00\"";
    EXPECT_EQ(why(k),
              "operation 'kernel-read': its routine isn't one its start lists")
        << "an erase's id is never the helper's start";
    k = Kernel();
    k.sha = std::string(64, '0');
    EXPECT_NE(why(k).find("has SHA-256 " + sha_of(k.blob)), std::string::npos);
    k = Kernel();
    k.kind = "\"dump\"";
    EXPECT_EQ(why(k), "operation 'kernel-read' is a dump, which takes no "
                      "\"kernel\"");
    k = Kernel();
    k.lead = k.routine_step() + ", " + k.download() + ", ";
    EXPECT_NE(why(k).find("first two steps"), std::string::npos)
        << "the routine before the download";
    k = Kernel();
    k.lead = k.download() + ", ";
    EXPECT_NE(why(k).find("first two steps"), std::string::npos)
        << "no routine";
    k = Kernel();
    k.ranges = "";
    k.lead = k.download() + ", " + k.routine_step();
    EXPECT_NE(why(k).find("a download, its routine, then the ranges"),
              std::string::npos);
    k = Kernel();
    k.start = "[]";
    EXPECT_NE(why(k).find("start lists 1 to 8 routines"), std::string::npos);
    k = Kernel();
    k.start = R"(["0201", "0202", "0203", "0204", "0205", "0206", "0207", )"
              R"("0208", "0209"])";
    EXPECT_NE(why(k).find("start lists 1 to 8 routines"), std::string::npos);
}

TEST(PluginOperationTest, AFunctionalRequestIsSuppressedAndOnlyOneOfTwo)
{
    const auto with = [](const std::string& functional) {
        Kernel k;
        k.functional = functional;
        return why(k);
    };
    const std::string refused =
        "a functional request is \"10 82\", or \"3E 80\" with its "
        "\"every_ms\", each once: nothing else goes to every module";
    for (const char* f :
         {R"([{"request": "10 02"}])",
          R"([{"request": "3E 00", "every_ms": 2000}])",
          R"([{"request": "11 81"}])", R"([{"request": "10 81"}])",
          R"([{"request": "10 82", "every_ms": 2000}])",
          R"([{"request": "3E 80"}])",
          R"([{"request": "10 82"}, {"request": "10 82"}])",
          R"([{"request": "3E 80", "every_ms": 2000}, )"
          R"({"request": "3E 80", "every_ms": 1000}])"}) {
        EXPECT_EQ(with(f), refused) << f;
    }
    EXPECT_NE(
        with(R"([{"request": "3E 80", "every_ms": 499}])").find("every_ms"),
        std::string::npos);
    EXPECT_NE(
        with(R"([{"request": "3E 80", "every_ms": 4501}])").find("every_ms"),
        std::string::npos);
    EXPECT_NE(with(R"([{"request": "10 82", "to": "physical"}])")
                  .find("unknown key 'to'"),
              std::string::npos)
        << "a functional request names no address: it is the functional one";
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    Kernel k;
    k.functional = "";
    const Plugin none = load_packed(made(k).json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_plugin_operation_kernel_t out{};
    out.struct_size = sizeof out;
    ASSERT_EQ(ecuio_plugin_operation_kernel(none.get(), 0, &out), ECUIO_OK);
    EXPECT_EQ(out.quiet_session, 0);
    EXPECT_EQ(out.quiet_keepalive, 0);
    EXPECT_EQ(out.keepalive_ms, 0u);
}

TEST(PluginOperationTest, AHelpersFileIsABinFileTheManifestNames)
{
    Kernel k;
    Made m = made(k);
    m.files_extra = R"(, "kernel.bin": {"kind": "wasm", "base64": ")" +
                    base64(k.blob) + "\"}";
    EXPECT_EQ(why(m),
              "file 'kernel.bin''s kind is 'wasm': its step reads \"bin\"");
    m.files_extra = R"(, "kernel.bin": {"kind": "elf", "base64": ")" +
                    base64(k.blob) + "\"}";
    EXPECT_EQ(why(m), "a binary file's kind 'elf' isn't one this Havne reads: "
                      "\"wasm\" and \"bin\"");
    m = made(k);
    m.files_extra += R"(, "spare.bin": {"kind": "bin", "base64": "AAAA"})";
    EXPECT_EQ(why(m), "file 'spare.bin' is a binary file nothing in the "
                      "manifest names");
    m = made(k);
    m.files_extra = "";
    EXPECT_EQ(why(m), "file 'kernel.bin' isn't a binary file in \"files\"");
}

TEST(PluginOperationTest, AKeepAlivesTimeGoesWithItsFunctional3E80)
{
    ecuio_operation::plan p;
    p.has_kernel = true;
    ecuio_operation::kernel& k = p.helper;
    k.ram_address = 0x00F00000;
    k.ram_bytes = 0x4000;
    k.address = k.ram_address;
    k.blob.assign(300, 0x5A);
    k.starts = {0x0201};
    k.routine = 0x0201;
    EXPECT_EQ(ecuio_operation::kernel_fault(p), nullptr);
    k.keepalive_ms = 2000;
    ASSERT_NE(ecuio_operation::kernel_fault(p), nullptr);
    EXPECT_STREQ(ecuio_operation::kernel_fault(p),
                 "a keep-alive's every_ms goes with a functional 3E 80");
    k.quiet_keepalive = 0x80;
    EXPECT_EQ(ecuio_operation::kernel_fault(p), nullptr);
}

TEST(PluginOperationTest, AHelpersDfiAndStartOptionReadBack)
{
    Kernel k;
    k.download_extra = R"(, "dfi": "01")";
    k.routine_extra = R"(, "option": "A55A0FF0")";
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    const Plugin p = load_packed(made(k).json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    ecuio_plugin_operation_step_t st{};
    st.struct_size = sizeof st;
    ASSERT_EQ(ecuio_plugin_operation_step(p.get(), 0, 0, &st), ECUIO_OK);
    EXPECT_EQ(st.dfi, 0x01);
    EXPECT_EQ(st.option_len, 0u);
    ASSERT_EQ(ecuio_plugin_operation_step(p.get(), 0, 1, &st), ECUIO_OK);
    EXPECT_EQ(st.option_len, 4u);
    EXPECT_EQ(st.dfi, 0);
    ASSERT_EQ(ecuio_plugin_operation_step(p.get(), 0, 2, &st), ECUIO_OK);
    EXPECT_EQ(st.dfi, 0) << "the upload stays plain";

    size_t count = 99;
    EXPECT_EQ(
        ecuio_plugin_operation_step_option(p.get(), 0, 1, nullptr, 0, &count),
        ECUIO_OK);
    EXPECT_EQ(count, 4u);
    uint8_t got[ECUIO_PLUGIN_MAX_OPTION] = {};
    count = 0;
    EXPECT_EQ(ecuio_plugin_operation_step_option(p.get(), 0, 1, got, 2, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 4u) << "all of it counted, the first two copied";
    EXPECT_EQ(got[0], 0xA5);
    EXPECT_EQ(got[1], 0x5A);
    EXPECT_EQ(got[2], 0);
    ASSERT_EQ(ecuio_plugin_operation_step_option(p.get(), 0, 1, got, sizeof got,
                                                 &count),
              ECUIO_OK);
    EXPECT_EQ(std::vector<uint8_t>(got, got + count),
              (std::vector<uint8_t>{0xA5, 0x5A, 0x0F, 0xF0}));
    for (const uint32_t step : {0u, 2u}) {
        count = 99;
        EXPECT_EQ(ecuio_plugin_operation_step_option(p.get(), 0, step, got,
                                                     sizeof got, &count),
                  ECUIO_OK);
        EXPECT_EQ(count, 0u) << step;
    }
    EXPECT_EQ(ecuio_plugin_operation_step_option(nullptr, 0, 1, got, sizeof got,
                                                 &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step_option(p.get(), 0, 1, got, sizeof got,
                                                 nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(
        ecuio_plugin_operation_step_option(p.get(), 0, 1, nullptr, 4, &count),
        ECUIO_ERR_NULL_ARGUMENT);
    count = 7;
    EXPECT_EQ(ecuio_plugin_operation_step_option(p.get(), 0, 3, got, sizeof got,
                                                 &count),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step_option(p.get(), 1, 1, got, sizeof got,
                                                 &count),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(count, 7u) << "out untouched";

    const Plugin plain = load_packed(made(Kernel()).json(), &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    ASSERT_EQ(ecuio_plugin_operation_step(plain.get(), 0, 1, &st), ECUIO_OK);
    EXPECT_EQ(st.option_len, 0u);
    EXPECT_EQ(ecuio_plugin_operation_step_option(plain.get(), 0, 1, got,
                                                 sizeof got, &count),
              ECUIO_OK);
    EXPECT_EQ(count, 0u) << "a bare 31 01 <rid>";
}

TEST(PluginOperationTest, AStartOptionIsOneTo16BytesAndADfiOneByte)
{
    Kernel k;
    k.routine_extra = R"(, "option": "00112233445566778899AABBCCDDEEFF")";
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    EXPECT_NE(load_packed(made(k).json(), &s), nullptr) << "16 bytes";
    EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    for (const char* option :
         {R"("")", R"("00112233445566778899AABBCCDDEEFF00")"}) {
        k.routine_extra = std::string(R"(, "option": )") + option;
        EXPECT_NE(why(k).find("'start''s option"), std::string::npos)
            << option << ": " << why(k);
    }
    for (const char* option : {R"("A55")", R"("a55a")", R"("A5 5A")"}) {
        k.routine_extra = std::string(R"(, "option": )") + option;
        EXPECT_NE(why(k).find("takes pairs of hex digits"), std::string::npos)
            << option << ": " << why(k);
    }
    k = Kernel();
    k.routine_extra = R"(, "dfi": "01")";
    EXPECT_EQ(why(k), "unknown key 'dfi' in a routine");
    k = Kernel();
    k.download_extra = R"(, "option": "A5")";
    EXPECT_EQ(why(k), "unknown key 'option' in a download");
    k = Kernel();
    k.download_extra = R"(, "dfi": "0101")";
    EXPECT_NE(why(k).find("'helper''s dfi"), std::string::npos) << why(k);
    k = Kernel();
    k.download_extra = R"(, "dfi": "FF")";
    EXPECT_NE(load_packed(made(k).json(), &s), nullptr)
        << "any data format is the plugin's claim";
    EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
}
