// File/layer: tests/test_query_keys.cpp | test_core, Qt-free, C++17
// Testing: UNIT: QueryKeysTest (each transport's query keys against the
//          room create's refusal joins them into)
//
// A key that pushes a list past ECUIO_QUERY_KEYS_TEXT_MAX would cut the
// refusal's list of keys short: this fails first. Every list is defined
// on every platform, SocketCAN's too.

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/internal.h>

#include <string>

namespace {

/* core.cpp's join: "a", "a and b", "a, b and c" */
std::string joined(const char* const* keys)
{
    size_t n = 0;
    while (keys[n] != nullptr) {
        ++n;
    }
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += (i == 0) ? "" : (i + 1 == n) ? " and " : ", ";
        s += keys[i];
    }
    return s;
}

} // namespace

TEST(QueryKeysTest, EachTransportsKeysFitTheRefusalsRoom)
{
    const struct {
        const char* scheme;
        const char* const* keys;
    } lists[] = {
        {"mock://", ecuio_mock_query_keys},
        {"replay://", ecuio_replay_query_keys},
        {"elm327://", ecuio_elm327_query_keys},
        {"socketcan://", ecuio_socketcan_query_keys},
        {"j2534://", ecuio_j2534_query_keys},
    };
    for (const auto& l : lists) {
        const std::string text = joined(l.keys);
        EXPECT_FALSE(text.empty()) << l.scheme;
        EXPECT_LT(text.size(), static_cast<size_t>(ECUIO_QUERY_KEYS_TEXT_MAX))
            << l.scheme << " lists " << text;
    }
}

/* The join measured above is the one the refusal writes */
TEST(QueryKeysTest, TheRefusalListsTheKeysAsJoinedHere)
{
    ecuio_session_t* s = nullptr;
    EXPECT_EQ(ecuio_session_create("mock://x?interval=10", &s),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(s, nullptr);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              "mock:// takes no query key 'interval', only " +
                  joined(ecuio_mock_query_keys));
}
