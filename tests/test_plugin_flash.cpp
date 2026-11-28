#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstring>
#include <string>

#include "plugin_flash_support.h"

namespace {

using conversation_test::bytes;
using conversation_test::Folder;
using conversation_test::folder_of;
using conversation_test::load_folder;
using conversation_test::load_packed;
using conversation_test::Made;
using conversation_test::packed_refusal;
using conversation_test::Plugin;

using flash_test::Flash;
using flash_test::Image;
using flash_test::made;

std::string why(const Flash& f)
{
    const std::string words = packed_refusal(made(f));
    const size_t at = words.find(": ");
    return at == std::string::npos ? words : words.substr(at + 2);
}

void expect_the_flash(const ecuio_plugin_t* p, const bytes& blob)
{
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    ASSERT_EQ(ecuio_plugin_operation(p, 0, &o), ECUIO_OK);
    EXPECT_EQ(o.kind, ECUIO_OPERATION_KIND_FLASH);
    EXPECT_EQ(o.op_class, ECUIO_OPERATION_CLASS_WRITE);
    EXPECT_EQ(o.step_count, 1u);
    EXPECT_EQ(o.image_bytes, blob.size());
    EXPECT_EQ(o.leave, ECUIO_OPERATION_LEAVE_RESET);
    EXPECT_EQ(o.unlock_sessions, 1u);

    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    ASSERT_EQ(ecuio_plugin_operation_step(p, 0, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.kind, ECUIO_STEP_DOWNLOAD);
    EXPECT_EQ(s.session, ECUIO_STEP_SESSION_PROGRAMMING);
    EXPECT_EQ(s.address, 0x00080000u);
    EXPECT_EQ(s.bytes, blob.size());
    EXPECT_EQ(s.dfi, 0u);
    EXPECT_STREQ(s.name, "code");

    ecuio_plugin_operation_flash_t f{};
    f.struct_size = sizeof f;
    ASSERT_EQ(ecuio_plugin_operation_flash(p, 0, &f), ECUIO_OK);
    EXPECT_EQ(f.struct_size, sizeof f);
    EXPECT_EQ(f.flags, 0u);
    EXPECT_EQ(f.erase_routine, 0xFF00u);
    EXPECT_EQ(f.check_routine, 0xFF01u);
    EXPECT_EQ(f.pass_len, 1u);
    EXPECT_EQ(f.pass[0], 0x00u);
    EXPECT_EQ(f.sector_count, 2u);

    ecuio_plugin_operation_sector_t c{};
    c.struct_size = sizeof c;
    ASSERT_EQ(ecuio_plugin_operation_sector(p, 0, 1, &c), ECUIO_OK);
    EXPECT_EQ(c.address, 0x00080100u);
    EXPECT_EQ(c.bytes, 256u);

    const uint8_t* got = nullptr;
    uint64_t length = 0;
    ASSERT_EQ(ecuio_plugin_operation_step_blob(p, 0, 0, &got, &length),
              ECUIO_OK);
    ASSERT_EQ(length, blob.size());
    EXPECT_EQ(std::memcmp(got, blob.data(), blob.size()), 0);
}

Plugin loaded(const Flash& f)
{
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    Plugin p = load_packed(made(f).json(), &s);
    EXPECT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    return p;
}

} // namespace

TEST(PluginOperationTest, AFlashReadsBackPackedAndFromAFolder)
{
    const Flash f;
    const Plugin packed = loaded(f);
    ASSERT_NE(packed.get(), nullptr);
    expect_the_flash(packed.get(), f.images[0].blob);
    Folder folder = folder_of(made(f));
    folder.files["code.bin"] =
        std::string(f.images[0].blob.begin(), f.images[0].blob.end());
    ecuio_status_t s = ECUIO_ERR_NULL_ARGUMENT;
    const Plugin from_folder = load_folder(folder, &s);
    ASSERT_EQ(s, ECUIO_OK) << ecuio_last_error_detail();
    expect_the_flash(from_folder.get(), f.images[0].blob);

    ecuio_plugin_fingerprint_t a{};
    a.struct_size = sizeof a;
    ecuio_plugin_fingerprint_t b{};
    b.struct_size = sizeof b;
    ASSERT_EQ(ecuio_plugin_fingerprint(packed.get(), &a), ECUIO_OK);
    Flash other;
    other.images[0].blob[300] ^= 0x01;
    const Plugin changed = loaded(other);
    ASSERT_NE(changed.get(), nullptr);
    ASSERT_EQ(ecuio_plugin_fingerprint(changed.get(), &b), ECUIO_OK);
    EXPECT_NE(std::memcmp(a.fingerprint, b.fingerprint, sizeof a.fingerprint),
              0)
        << "a consent covers the image's bytes";
}

TEST(PluginOperationTest, AFlashsDeclarationsReadBack)
{
    Flash f;
    f.pass = "\"A5000102\"";
    f.flash_extra = R"(, "overwrite_safe": true)";
    f.images[0].extra = R"(, "dfi": "11")";
    f.op_extra.clear();
    const Plugin p = loaded(f);
    ASSERT_NE(p.get(), nullptr);
    ecuio_plugin_operation_flash_t got{};
    got.struct_size = sizeof got;
    ASSERT_EQ(ecuio_plugin_operation_flash(p.get(), 0, &got), ECUIO_OK);
    EXPECT_EQ(got.flags, ECUIO_OPERATION_FLASH_OVERWRITE_SAFE);
    ASSERT_EQ(got.pass_len, 4u);
    EXPECT_EQ(got.pass[0], 0xA5u);
    EXPECT_EQ(got.pass[1], 0x00u);
    EXPECT_EQ(got.pass[2], 0x01u);
    EXPECT_EQ(got.pass[3], 0x02u);
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    ASSERT_EQ(ecuio_plugin_operation_step(p.get(), 0, 0, &s), ECUIO_OK);
    EXPECT_EQ(s.dfi, 0x11u);
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    ASSERT_EQ(ecuio_plugin_operation(p.get(), 0, &o), ECUIO_OK);
    EXPECT_EQ(o.leave, ECUIO_OPERATION_LEAVE_RESET)
        << "a flash left with no leave still resets";
}

TEST(PluginOperationTest, TheFlashGettersCheckTheirArguments)
{
    const Plugin p = loaded(Flash());
    ASSERT_NE(p.get(), nullptr);
    ecuio_plugin_operation_flash_t f{};
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_operation_flash(nullptr, 0, &f),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_flash(p.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_flash(p.get(), 1, &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    f.struct_size = sizeof f - 1;
    EXPECT_EQ(ecuio_plugin_operation_flash(p.get(), 0, &f),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(f.sector_count, 0u) << "out untouched";

    ecuio_plugin_operation_sector_t c{};
    c.struct_size = sizeof c;
    EXPECT_EQ(ecuio_plugin_operation_sector(nullptr, 0, 0, &c),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_sector(p.get(), 0, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_sector(p.get(), 0, 2, &c),
              ECUIO_ERR_INVALID_ARGUMENT);
    c.struct_size = sizeof c - 1;
    EXPECT_EQ(ecuio_plugin_operation_sector(p.get(), 0, 0, &c),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(c.bytes, 0u) << "out untouched";

    const uint8_t* got = nullptr;
    uint64_t length = 7;
    EXPECT_EQ(ecuio_plugin_operation_step_blob(nullptr, 0, 0, &got, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step_blob(p.get(), 0, 0, nullptr, &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step_blob(p.get(), 0, 0, &got, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_operation_step_blob(p.get(), 0, 1, &got, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(length, 7u);

    const Plugin k =
        load_packed(kernel_test::made(kernel_test::Kernel()).json());
    ASSERT_NE(k.get(), nullptr);
    f.struct_size = sizeof f;
    EXPECT_EQ(ecuio_plugin_operation_flash(k.get(), 0, &f),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "a kernel read is no flash";
    EXPECT_EQ(ecuio_plugin_operation_step_blob(k.get(), 0, 0, &got, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
}

TEST(PluginOperationTest, AFlashsRefusalsSayWhy)
{
    Flash f;
    f.check = "\"FF00\"";
    EXPECT_EQ(why(f), "operation 'flash-code': its erase and its check are "
                      "one routine");

    f = Flash();
    f.sectors = R"([{"address": "00080100", "bytes": 256}, )"
                R"({"address": "00080000", "bytes": 256}])";
    EXPECT_EQ(why(f), "operation 'flash-code': its sectors are in the order "
                      "of their addresses, none sharing one");

    f = Flash();
    f.sectors = R"([{"address": "00080000", "bytes": 257}, )"
                R"({"address": "00080100", "bytes": 256}])";
    EXPECT_EQ(why(f), "operation 'flash-code': its sectors are in the order "
                      "of their addresses, none sharing one");

    f = Flash();
    f.sectors = R"([{"address": "00080000", "bytes": 256}, )"
                R"({"address": "00080100", "bytes": 257}])";
    EXPECT_EQ(why(f), "operation 'flash-code': a sector runs past the end of "
                      "its download");

    f = Flash();
    f.sectors = R"([{"address": "00080000", "bytes": 256}])";
    EXPECT_EQ(why(f), "operation 'flash-code': its sectors don't cover each "
                      "download whole");

    f = Flash();
    f.sectors = R"([{"address": "00070000", "bytes": 16}, )"
                R"({"address": "00080000", "bytes": 256}, )"
                R"({"address": "00080100", "bytes": 256}])";
    EXPECT_EQ(why(f), "operation 'flash-code': a sector lies outside every "
                      "download");

    f = Flash();
    Image second;
    second.name = "more";
    second.file = "more.bin";
    second.address = "00080100";
    f.images.push_back(second);
    f.sectors = R"([{"address": "00080000", "bytes": 512}])";
    EXPECT_EQ(why(f), "operation 'flash-code': two of its downloads share an "
                      "address");

    f = Flash();
    f.images[0].address = "FFFFFF00";
    f.sectors = R"([{"address": "FFFFFF00", "bytes": 512}])";
    EXPECT_EQ(why(f), "operation 'flash-code': an image doesn't fit its "
                      "address_bytes and size_bytes");

    f = Flash();
    f.images[0].sha = std::string(64, '0');
    EXPECT_NE(why(f).find("file 'code.bin' has SHA-256 "), std::string::npos)
        << why(f);
}

TEST(PluginOperationTest, AFlashsShapeIsItsOwn)
{
    Flash f;
    f.op_extra = R"(, "leave": "default")";
    EXPECT_EQ(why(f), "operation 'flash-code' is a flash, which ends with "
                      "\"reset\" (11 01)");

    f = Flash();
    f.op_extra = R"(, "ident": [{"did": "D220", "role": "hardware"}])";
    EXPECT_EQ(why(f), "operation 'flash-code' is a flash: this Havne reads "
                      "no identifiers around one");

    f = Flash();
    f.op_extra = R"(, "kernel": {})";
    EXPECT_EQ(why(f), "operation 'flash-code' is a flash, which takes no "
                      "\"kernel\"");

    f = Flash();
    f.kind = "\"dump\"";
    EXPECT_EQ(why(f), "operation 'flash-code' isn't a flash, which alone "
                      "takes \"flash\"");

    f = Flash();
    f.images[0].extra = R"(, "chunk": 16)";
    EXPECT_NE(why(f).find("a flash's step"), std::string::npos) << why(f);

    f = Flash();
    f.pass = "\"0000000000\"";
    EXPECT_NE(why(f).find("the flash's pass"), std::string::npos) << why(f);

    f = Flash();
    f.sectors = "[]";
    EXPECT_EQ(why(f), "the flash's sectors lists 1 to 64");
}
