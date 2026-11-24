// File/layer: tests/test_consent_lines.cpp | test-only, C++17, Qt-free
// Testing: UNIT: ConsentLinesTest (test_core)
//
// Consent's lines (consent_lines.cpp): each set's lines in their order,
// the read set holding the conversation set's, the write class refused,
// and every text plain keyboard text in American English (the docs lint
// reads no C++ string).

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<ecuio_consent_line_t> lines_of(uint32_t set)
{
    std::vector<ecuio_consent_line_t> out;
    for (uint32_t i = 0;; ++i) {
        ecuio_consent_line_t l;
        std::memset(&l, 0xA5, sizeof l);
        l.struct_size = sizeof l;
        const ecuio_status_t s = ecuio_consent_line(set, i, &l);
        if (s != ECUIO_OK) {
            EXPECT_EQ(s, ECUIO_ERR_INVALID_ARGUMENT);
            EXPECT_EQ(l.id, 0xA5A5A5A5u) << "out untouched past the last";
            break;
        }
        EXPECT_EQ(l.struct_size, sizeof l);
        EXPECT_EQ(l.reserved0, 0u);
        for (const uint32_t r : l.reserved) {
            EXPECT_EQ(r, 0u);
        }
        out.push_back(l);
        if (i > 64) {
            ADD_FAILURE() << "no end";
            break;
        }
    }
    return out;
}

std::vector<uint32_t> ids_of(uint32_t set)
{
    std::vector<uint32_t> ids;
    for (const ecuio_consent_line_t& l : lines_of(set)) {
        ids.push_back(l.id);
    }
    return ids;
}

const std::vector<uint32_t> kConversation = {
    ECUIO_CONSENT_LINE_HAVNE_1,      ECUIO_CONSENT_LINE_HAVNE_2,
    ECUIO_CONSENT_LINE_HAVNE_3,      ECUIO_CONSENT_LINE_HAVNE_4,
    ECUIO_CONSENT_LINE_HAVNE_5,      ECUIO_CONSENT_LINE_NOT_A_CONFIRMATION,
    ECUIO_CONSENT_LINE_ENGINE_STATE, ECUIO_CONSENT_LINE_LOCKOUT,
};

const std::vector<uint32_t> kRead = {
    ECUIO_CONSENT_LINE_HAVNE_1,        ECUIO_CONSENT_LINE_HAVNE_2,
    ECUIO_CONSENT_LINE_HAVNE_3,        ECUIO_CONSENT_LINE_HAVNE_4,
    ECUIO_CONSENT_LINE_HAVNE_5,        ECUIO_CONSENT_LINE_NOT_A_CONFIRMATION,
    ECUIO_CONSENT_LINE_EMISSIONS_READ, ECUIO_CONSENT_LINE_WARRANTY_READ,
    ECUIO_CONSENT_LINE_ENGINE_STATE,   ECUIO_CONSENT_LINE_LOCKOUT,
    ECUIO_CONSENT_LINE_COPY_DATA,      ECUIO_CONSENT_LINE_COPYRIGHT,
};

size_t count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos;
         at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

} // namespace

TEST(ConsentLinesTest, EachSetsLinesAreInTheirOrder)
{
    EXPECT_EQ(ids_of(ECUIO_CONSENT_SET_CONVERSATION), kConversation);
    EXPECT_EQ(ids_of(ECUIO_CONSENT_SET_READ), kRead);
    EXPECT_EQ(ids_of(ECUIO_CONSENT_SET_CONVERSATION | ECUIO_CONSENT_SET_READ),
              kRead)
        << "the read set holds the conversation set's lines";

    /* The action lines, and the headings only the exposure lines have */
    for (const ecuio_consent_line_t& l : lines_of(ECUIO_CONSENT_SET_READ)) {
        const bool action = l.id == ECUIO_CONSENT_LINE_ENGINE_STATE ||
                            l.id == ECUIO_CONSENT_LINE_LOCKOUT;
        EXPECT_EQ(l.flags, action ? ECUIO_CONSENT_LINE_ACTION : 0u) << l.key;
        const bool exposure = l.id == ECUIO_CONSENT_LINE_EMISSIONS_READ ||
                              l.id == ECUIO_CONSENT_LINE_WARRANTY_READ ||
                              l.id == ECUIO_CONSENT_LINE_COPY_DATA ||
                              l.id == ECUIO_CONSENT_LINE_COPYRIGHT;
        EXPECT_EQ(std::string(l.heading).empty(), !exposure) << l.key;
    }
    const ecuio_consent_line_t lockout =
        lines_of(ECUIO_CONSENT_SET_CONVERSATION).back();
    EXPECT_STREQ(lockout.key, "lockout");
    EXPECT_EQ(std::string(lockout.text).find('['), std::string::npos)
        << "the count's sentences are lockout-count's, never consent's";
}

TEST(ConsentLinesTest, NoSetLeavesTheWriteClassToAPartialText)
{
    ecuio_consent_line_t l;
    std::memset(&l, 0xA5, sizeof l);
    l.struct_size = sizeof l;
    for (const uint32_t set :
         {0u, ECUIO_CONSENT_SET_WRITE,
          ECUIO_CONSENT_SET_WRITE | ECUIO_CONSENT_SET_CONVERSATION, 0x8u}) {
        EXPECT_EQ(ecuio_consent_line(set, 0, &l), ECUIO_ERR_INVALID_ARGUMENT)
            << set;
    }
    EXPECT_EQ(l.id, 0xA5A5A5A5u);
    l.struct_size = sizeof l - 1;
    EXPECT_EQ(ecuio_consent_line(ECUIO_CONSENT_SET_CONVERSATION, 0, &l),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_consent_line(ECUIO_CONSENT_SET_CONVERSATION, 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    /* The write's own lines are in no set this core fills */
    for (const uint32_t set :
         {ECUIO_CONSENT_SET_CONVERSATION, ECUIO_CONSENT_SET_READ}) {
        for (const uint32_t id : ids_of(set)) {
            EXPECT_NE(id, ECUIO_CONSENT_LINE_WRITE_ON_ASK);
            EXPECT_NE(id, ECUIO_CONSENT_LINE_LOCKOUT_COUNT);
            EXPECT_NE(id, ECUIO_CONSENT_LINE_EMISSIONS);
        }
    }
}

TEST(ConsentLinesTest, EveryLineIsPlainKeyboardText)
{
    /* The docs lint's British spellings that could reach these words,
     * split so the lint passes this list by */
    const char* british[] = {"co"
                             "lour",
                             "be"
                             "haviour",
                             "li"
                             "cence",
                             "pr"
                             "ogramme",
                             "an"
                             "alys",
                             "ce"
                             "ntre",
                             "or"
                             "ganis",
                             "fa"
                             "vour",
                             "re"
                             "cognis",
                             "ty"
                             "re",
                             "au"
                             "thoris",
                             "ho"
                             "nour"};
    for (const ecuio_consent_line_t& l : lines_of(ECUIO_CONSENT_SET_READ)) {
        for (const char* t : {l.key, l.text, l.heading}) {
            const std::string s(t);
            for (const char c : s) {
                const auto u = static_cast<unsigned char>(c);
                EXPECT_TRUE(u >= 0x20 && u <= 0x7E) << l.key << ": " << s;
                EXPECT_TRUE(c != '<' && c != '&') << l.key;
            }
            EXPECT_EQ(s.find("--"), std::string::npos) << l.key;
            for (const char* b : british) {
                EXPECT_EQ(s.find(b), std::string::npos) << l.key << ": " << b;
            }
        }
        EXPECT_FALSE(std::string(l.text).empty()) << l.key;
        EXPECT_FALSE(std::string(l.key).empty());
    }
    const ecuio_consent_line_t first =
        lines_of(ECUIO_CONSENT_SET_CONVERSATION).front();
    EXPECT_EQ(count(first.text, "{name}"), 1u);
    EXPECT_EQ(count(first.text, "{version}"), 1u);
    for (const ecuio_consent_line_t& l : lines_of(ECUIO_CONSENT_SET_READ)) {
        if (l.id != ECUIO_CONSENT_LINE_HAVNE_1) {
            EXPECT_EQ(std::string(l.text).find('{'), std::string::npos)
                << l.key;
        }
    }
}
