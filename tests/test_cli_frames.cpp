// File/layer: tests/test_cli_frames.cpp | test_cli, Qt-free, C++17
// Testing: UNIT: CliFramesTest (test_cli)
//
// cli/frames_lines.h: the frames command's arguments and its lines.

#include "frames_lines.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> words;
    std::vector<char*> argv;
    explicit Args(std::vector<std::string> w) : words(std::move(w))
    {
        for (std::string& s : words) {
            argv.push_back(&s[0]);
        }
    }
    int argc() const { return static_cast<int>(argv.size()); }
};

bool parsed(std::vector<std::string> w, cli::FramesArgs& out)
{
    Args a(std::move(w));
    std::string why;
    const bool ok = cli::parse_frames(a.argc(), a.argv.data(), out, &why);
    EXPECT_EQ(ok, why.empty()) << why;
    if (ok) {
        out.folder = nullptr; /* it pointed into the words */
    }
    return ok;
}

std::string why_of(std::vector<std::string> w)
{
    Args a(std::move(w));
    cli::FramesArgs fr;
    std::string why;
    EXPECT_FALSE(cli::parse_frames(a.argc(), a.argv.data(), fr, &why));
    return why;
}

std::string line_of(const ecuio_frame_t& f, uint64_t host_us)
{
    char buf[16 * 1024];
    cli::Line l(buf, sizeof buf);
    cli::frame_row_line(l, f, host_us);
    EXPECT_TRUE(l.finish());
    return std::string(l.data(), l.size());
}

} // namespace

TEST(CliFramesTest, TheStretchIsInSecondsWithBothEdgesTaken)
{
    cli::FramesArgs fr;
    ASSERT_TRUE(parsed({"havne-cli", "frames", "rec"}, fr));
    EXPECT_EQ(fr.from_us, 0U);
    EXPECT_EQ(fr.to_us, 0U);
    ASSERT_TRUE(parsed(
        {"havne-cli", "frames", "rec", "--from", "1.5", "--to", "2.25"}, fr));
    EXPECT_EQ(fr.from_us, 1500000U);
    EXPECT_EQ(fr.to_us, 2250000U);
    /* One instant: both edges are the same time */
    cli::FramesArgs one;
    ASSERT_TRUE(parsed(
        {"havne-cli", "frames", "--from", "3", "rec", "--to", "3"}, one));
    EXPECT_EQ(one.from_us, 3000000U);
    EXPECT_EQ(one.to_us, 3000000U);
}

TEST(CliFramesTest, BadArgumentsSayWhy)
{
    EXPECT_EQ(why_of({"havne-cli", "frames"}),
              "frames needs a recording's folder");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "b"}),
              "frames takes one recording");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--to", "0"}),
              "--to takes seconds after 0");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--from", "2", "--to", "1"}),
              "--to comes before --from");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--from", "-1"}),
              "--from takes seconds, 0 or more");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--to", "soon"}),
              "--to takes seconds, 0 or more");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--raw"}),
              "unexpected '--raw'");
    EXPECT_EQ(why_of({"havne-cli", "frames", "a", "--from"}),
              "unexpected '--from'");
}

TEST(CliFramesTest, ARowsLineIsTheFrameAsRecordedWithItsHostTime)
{
    auto f = std::make_unique<ecuio_frame_t>();
    f->arbitration_id = 0x7E8;
    f->flags = ECUIO_FRAME_FLAG_REASSEMBLED;
    f->timestamp_us = 1026;
    f->data_length = 4;
    f->data[0] = 0x41;
    f->data[1] = 0x0C;
    f->data[2] = 0x0B;
    f->data[3] = 0xD7;
    EXPECT_EQ(
        line_of(*f, 20),
        "{\"type\":\"frame\",\"host_us\":20,\"t_us\":1026,\"id\":\"0x7E8\","
        "\"flags\":256,\"data\":\"41 0C 0B D7\"}\n");

    f->arbitration_id = 0x18DAF110;
    f->flags = ECUIO_FRAME_FLAG_EXTENDED;
    f->data_length = 0;
    EXPECT_EQ(line_of(*f, 7),
              "{\"type\":\"frame\",\"host_us\":7,\"t_us\":1026,"
              "\"id\":\"0x18DAF110\",\"flags\":1,\"data\":\"\"}\n");

    /* A whole frame's bytes fit the CLI's line */
    f->data_length = ECUIO_FRAME_DATA_MAX;
    const std::string whole = line_of(*f, 1);
    EXPECT_LT(whole.size(), 3U * ECUIO_FRAME_DATA_MAX + 128);
    EXPECT_EQ(whole.substr(whole.size() - 3), "\"}\n");
}

TEST(CliFramesTest, TheEndLineCountsTheRows)
{
    ecuio_frames_read_t s{};
    s.rows = 7074;
    s.torn_rows = 1;
    char buf[256];
    cli::Line l(buf, sizeof buf);
    cli::frames_end_line(l, "read", s);
    ASSERT_TRUE(l.finish());
    EXPECT_EQ(std::string(l.data(), l.size()),
              "{\"type\":\"end\",\"reason\":\"read\",\"rows\":7074,"
              "\"torn_rows\":1}\n");
}
