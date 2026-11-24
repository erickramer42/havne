// File/layer: tests/test_plugin_publisher.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: PluginPublisherTest (test_signals)
//
// Manifest format 3's "publisher" (plugin_publisher.cpp): required with
// the conversation tier, format 3's alone, each text at its bound and
// plain, the terms whole through a call that never cuts them, and no
// "signature" key.

#include "plugin_conversation_support.h"

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using conversation_test::Made;
using conversation_test::Plugin;

const char* const kTerms = "Use at your own risk.\n\tNo warranty.";

std::string block(const std::string& name = "Made-up Tools",
                  const std::string& contact = "https://example.com/help",
                  const std::string& license = "MIT",
                  const std::string& terms = "Use at your own risk.\\n\\tNo "
                                             "warranty.",
                  const std::string& extra = "")
{
    return R"({"name": ")" + name + R"(", "contact": ")" + contact +
           R"(", "license": ")" + license + R"(", "terms": ")" + terms + "\"" +
           extra + "}";
}

Made format3(const std::string& publisher = block())
{
    Made m;
    m.format = "3";
    m.publisher = publisher;
    return m;
}

std::string refusal(const Made& m)
{
    ecuio_status_t s = ECUIO_OK;
    const Plugin p = conversation_test::load_packed(m.json(), &s);
    EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
    return ecuio_last_error_detail();
}

} // namespace

TEST(PluginPublisherTest, AFormat3ConversationPluginNamesItsPublisher)
{
    const Plugin p = conversation_test::load_packed(format3().json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    ecuio_plugin_publisher_t pub;
    std::memset(&pub, 0xA5, sizeof pub);
    pub.struct_size = sizeof pub;
    ASSERT_EQ(ecuio_plugin_publisher(p.get(), &pub), ECUIO_OK);
    EXPECT_EQ(pub.struct_size, sizeof pub);
    EXPECT_STREQ(pub.name, "Made-up Tools");
    EXPECT_STREQ(pub.contact, "https://example.com/help");
    EXPECT_STREQ(pub.license, "MIT");
    EXPECT_EQ(pub.terms_len, std::strlen(kTerms));
    EXPECT_EQ(ecuio_sha256::hex(pub.terms_sha256),
              ecuio_sha256::hex_of(kTerms, std::strlen(kTerms)));
    EXPECT_EQ(pub.flags, 0u);
    for (const uint32_t r : pub.reserved) {
        EXPECT_EQ(r, 0u);
    }
    /* Every byte past each text is zero */
    for (size_t i = std::strlen(pub.contact); i < sizeof pub.contact; ++i) {
        EXPECT_EQ(pub.contact[i], '\0') << i;
    }

    /* As a folder too */
    conversation_test::Folder f = conversation_test::folder_of(format3());
    EXPECT_NE(conversation_test::load_folder(f), nullptr)
        << ecuio_last_error_detail();
}

TEST(PluginPublisherTest, EachRefusalIsInItsWords)
{
    struct row {
        const char* what;
        Made made;
        const char* words;
    };
    Made none = format3("");
    Made in2 = format3();
    in2.format = "2";
    Made in4 = format3();
    in4.format = "4";
    const std::string past_name(128, 'n');
    const std::string past_contact(256, 'c');
    const std::string past_license(64, 'l');
    const std::vector<row> rows = {
        {"no block with the tier", none,
         "a plugin with the conversation tier names its publisher"},
        {"the block in format 2", in2, "\"publisher\" is format 3's"},
        {"format 4", in4, "this Havne reads formats 1 to 3"},
        {"a block that isn't an object", format3("[]"), "\"publisher\""},
        {"no name",
         format3(R"({"contact": "c", "license": "l", "terms": "t"})"),
         "\"publisher\" needs \"name\""},
        {"no contact",
         format3(R"({"name": "n", "license": "l", "terms": "t"})"),
         "\"publisher\" needs \"contact\""},
        {"no license",
         format3(R"({"name": "n", "contact": "c", "terms": "t"})"),
         "\"publisher\" needs \"license\""},
        {"no terms",
         format3(R"({"name": "n", "contact": "c", "license": "l"})"),
         "\"publisher\" needs \"terms\""},
        {"an empty name", format3(block("")),
         "the publisher's name is 0 bytes; it takes 1 to 127"},
        {"an empty contact", format3(block("n", "")),
         "the publisher's contact is 0 bytes; it takes 1 to 255"},
        {"an empty license", format3(block("n", "c", "")),
         "the publisher's license is 0 bytes; it takes 1 to 63"},
        {"empty terms", format3(block("n", "c", "l", "")),
         "the publisher's terms is 0 bytes; it takes 1 to 65536"},
        {"a name past its bound", format3(block(past_name)),
         "the publisher's name is 128 bytes"},
        {"a contact past its bound", format3(block("n", past_contact)),
         "the publisher's contact is 256 bytes"},
        {"a license past its bound", format3(block("n", "c", past_license)),
         "the publisher's license is 64 bytes"},
        {"a key it doesn't take",
         format3(block("n", "c", "l", "t", R"(, "url": "u")")), "url"},
        {"a tab in the name", format3(block("a\\tb")),
         "the publisher's name holds a control character"},
        {"a carriage return in the terms",
         format3(block("n", "c", "l", "a\\rb")),
         "the publisher's terms holds a control character: only line feeds "
         "and tabs"},
        {"a delete in the license", format3(block("n", "c", "\\u007f")),
         "the publisher's license holds a control character"},
        {"a right-to-left override in the terms",
         format3(block("n", "c", "l", "a\\u202eb")),
         "the publisher's terms holds a character that shows as nothing or "
         "moves the text around it"},
        {"a zero-width space in the name", format3(block("a\\u200bb")),
         "the publisher's name holds a character that shows as nothing"},
        {"a NUL in the terms", format3(block("n", "c", "l", "a\\u0000b")),
         "the publisher's terms holds a NUL character"},
    };
    for (const row& r : rows) {
        const std::string words = refusal(r.made);
        EXPECT_NE(words.find(r.words), std::string::npos)
            << r.what << ": " << words;
    }

    /* A "signature" key, in any format, says what to do instead */
    Made signed3 = format3();
    signed3.ecus += R"(, "signature": {"alg": "ed25519"})";
    EXPECT_NE(refusal(signed3).find(
                  "Havne doesn't check signatures: check the publisher's "
                  "signature yourself, then remove the key"),
              std::string::npos);
}

TEST(PluginPublisherTest, TheTermsAreWholeAtTheirBoundAndRefusedPastIt)
{
    const std::string at(ECUIO_PLUGIN_TERMS_MAX, 't');
    const Plugin p = conversation_test::load_packed(
        format3(block("n", "c", "l", at)).json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    std::vector<char> buf(at.size() + 1, 'Z');
    size_t length = 0;
    ASSERT_EQ(ecuio_plugin_terms(p.get(), buf.data(), buf.size(), &length),
              ECUIO_OK);
    EXPECT_EQ(length, at.size());
    EXPECT_EQ(std::string(buf.data()), at);

    const std::string past(ECUIO_PLUGIN_TERMS_MAX + 1, 't');
    EXPECT_NE(refusal(format3(block("n", "c", "l", past)))
                  .find("the publisher's terms is 65537 bytes; it takes 1 to "
                        "65536"),
              std::string::npos);
}

TEST(PluginPublisherTest, TheTermsCallNeverCutsThem)
{
    const Plugin p = conversation_test::load_packed(format3().json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    const size_t whole = std::strlen(kTerms);

    size_t length = 0;
    EXPECT_EQ(ecuio_plugin_terms(p.get(), nullptr, 0, &length), ECUIO_OK);
    EXPECT_EQ(length, whole);

    std::vector<char> buf(whole + 1, 'Z');
    length = 0;
    EXPECT_EQ(ecuio_plugin_terms(p.get(), buf.data(), whole, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(length, whole);
    EXPECT_EQ(std::string(buf.begin(), buf.end()), std::string(whole + 1, 'Z'));

    EXPECT_EQ(ecuio_plugin_terms(p.get(), buf.data(), buf.size(), &length),
              ECUIO_OK);
    EXPECT_STREQ(buf.data(), kTerms);

    EXPECT_EQ(ecuio_plugin_terms(nullptr, buf.data(), buf.size(), &length),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_terms(p.get(), buf.data(), buf.size(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_terms(p.get(), nullptr, 4, &length),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(PluginPublisherTest, AFormat2ConversationPluginLoadsAndNamesNone)
{
    const Plugin p = conversation_test::load_packed(Made().json());
    ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
    ecuio_plugin_publisher_t pub;
    std::memset(&pub, 0xA5, sizeof pub);
    pub.struct_size = sizeof pub;
    EXPECT_EQ(ecuio_plugin_publisher(p.get(), &pub),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(pub.terms_len, 0xA5A5A5A5u) << "out untouched";
    size_t length = 7;
    EXPECT_EQ(ecuio_plugin_terms(p.get(), nullptr, 0, &length),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(length, 7u);
}

TEST(PluginPublisherTest, AFormat3DataPluginMayNameOneOrNot)
{
    for (const std::string& publisher : {std::string(), block()}) {
        Made m = format3(publisher);
        m.tiers = R"(["data"])";
        m.has_group = false;
        m.has_binary = false;
        const Plugin p = conversation_test::load_packed(m.json());
        ASSERT_NE(p, nullptr) << ecuio_last_error_detail();
        ecuio_plugin_publisher_t pub;
        std::memset(&pub, 0, sizeof pub);
        pub.struct_size = sizeof pub;
        EXPECT_EQ(ecuio_plugin_publisher(p.get(), &pub),
                  publisher.empty() ? ECUIO_ERR_INVALID_ARGUMENT : ECUIO_OK);
    }
}

TEST(PluginPublisherTest, TheGetterRefusesAShortStruct)
{
    const Plugin p = conversation_test::load_packed(format3().json());
    ASSERT_NE(p, nullptr);
    ecuio_plugin_publisher_t pub;
    std::memset(&pub, 0xA5, sizeof pub);
    pub.struct_size = sizeof pub - 1;
    EXPECT_EQ(ecuio_plugin_publisher(p.get(), &pub),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(pub.terms_len, 0xA5A5A5A5u);
    EXPECT_EQ(ecuio_plugin_publisher(nullptr, &pub), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_plugin_publisher(p.get(), nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}
