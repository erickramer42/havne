// File/layer: tests/test_elm327.cpp | GTest, UNIT tier, headless CI job
// Testing: UNIT: the ELM327 wire (core/src/elm327_protocol.h, ADR-0026
//          §3) over canned transcripts: lines in any chunking, every line
//          kind, frame shapes in both addressing modes, ISO-TP
//          reassembly per CAN ID, the voltage and the version strings,
//          the commands it formats, and replies reaching the request
//          engine; a WiFi adapter's address and a failed connect's words
//          (elm327_tcp.h). Qt-free.

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "elm327_protocol.h" /* core/src */
#include "elm327_tcp.h"      /* core/src: a WiFi adapter's address */
#include "request_engine.h"  /* core/src: replies reaching the engine */

namespace {

/* What a stream of bytes gave: each line (or "<prompt>") in order. */
std::vector<std::string> events_of(elm327_reader& r, const std::string& bytes)
{
    std::vector<std::string> out;
    for (const char c : bytes) {
        const elm327_step s = elm327_take(r, static_cast<uint8_t>(c));
        if (s.line) {
            out.emplace_back(std::string(r.line, r.len) +
                             (r.overflow ? "<overflow>" : ""));
        }
        if (s.prompt) {
            out.emplace_back("<prompt>");
        }
    }
    return out;
}

/* The messages a reply's lines reassemble to, as "ID: hex bytes". */
struct Reassembled {
    std::vector<std::string> messages;
    uint32_t lines = 0;
    uint32_t prompts = 0;
};

std::string hex_of(const uint8_t* d, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += elm327_hex_digit(d[i] >> 4);
        s += elm327_hex_digit(d[i]);
        if (i + 1 < n) {
            s += ' ';
        }
    }
    return s;
}

std::string message_text(const elm327_message& m)
{
    std::string id;
    for (int d = 2; d >= 0; --d) {
        id += elm327_hex_digit(m.id >> (4 * d));
    }
    return id + ": " + hex_of(m.data, m.len);
}

/* Feeds bytes in the chunks given (their sizes, in order; the rest in one
 * last chunk), classifying each line as an 11-bit reply. */
Reassembled reassemble(const std::string& bytes,
                       const std::vector<size_t>& chunks)
{
    elm327_reader r;
    static elm327_isotp iso; /* 32 KB: kept off the stack */
    iso = elm327_isotp();
    Reassembled out;
    size_t at = 0;
    auto feed = [&](size_t n) {
        for (size_t i = 0; i < n; ++i, ++at) {
            const elm327_step s =
                elm327_take(r, static_cast<uint8_t>(bytes[at]));
            if (s.line) {
                ++out.lines;
                elm327_can_line f;
                if (elm327_classify(r, false, &f) == elm327_line::frame) {
                    elm327_message m;
                    if (elm327_isotp_take(iso, f, &m) ==
                        elm327_isotp_result::complete) {
                        out.messages.push_back(message_text(m));
                    }
                }
            }
            if (s.prompt) {
                ++out.prompts;
                elm327_isotp_end_reply(iso);
            }
        }
    };
    for (const size_t n : chunks) {
        feed(n);
    }
    feed(bytes.size() - at);
    return out;
}

/* Parses a normalized-or-not frame line in a mode; fails the test if it
 * isn't one. */
elm327_can_line frame_of(const char* line, bool ext29 = false)
{
    elm327_can_line f;
    EXPECT_EQ(elm327_classify(line, strlen(line), false, ext29, &f),
              elm327_line::frame)
        << line;
    return f;
}

elm327_line kind_of(const char* line, bool ext29 = false)
{
    return elm327_classify(line, strlen(line), false, ext29, nullptr);
}

/* The datasheet's 09 04 example with headers on: two ECUs' replies
 * interleaved, 7E9's second Consecutive Frame after 7E8's. */
const std::string kInterleaved = "7E8 10 13 49 04 01 35 36 30\r"
                                 "7E8 21 32 38 39 34 39 41 43\r"
                                 "7E9 10 13 49 04 01 35 36 30\r"
                                 "7E8 22 00 00 00 00 00 00 31\r"
                                 "7E9 21 32 38 39 35 34 41 43\r"
                                 "7E9 22 00 00 00 00 00 00 00\r"
                                 "\r>";

const std::vector<std::string> kInterleavedMessages = {
    "7E8: 49 04 01 35 36 30 32 38 39 34 39 41 43 00 00 00 00 00 00",
    "7E9: 49 04 01 35 36 30 32 38 39 35 34 41 43 00 00 00 00 00 00",
};

} // namespace

/* ---- Lines ---------------------------------------------------------- */

/* CR ends a line, LF and NUL are nothing, empty lines are skipped, and
 * '>' is the prompt, ending a line too when no CR came before it. */
TEST(Elm327ProtocolTest, LinesEndAtCrAndThePromptStandsAlone)
{
    elm327_reader r;
    EXPECT_EQ(events_of(r, "ELM327 v1.4b\r\n\r\n>"),
              (std::vector<std::string>{"ELM327 v1.4b", "<prompt>"}));
    EXPECT_EQ(events_of(r, std::string("\0\0OK\r\r>", 7)),
              (std::vector<std::string>{"OK", "<prompt>"}));
    EXPECT_EQ(events_of(r, "OK>"), (std::vector<std::string>{"OK", "<prompt>"}))
        << "a prompt straight after text ends the line";
    EXPECT_EQ(events_of(r, ">"), (std::vector<std::string>{"<prompt>"}));
}

/* A line longer than ELM327_LINE_MAX keeps its start, is flagged, and
 * reads as malformed; the next line is whole. */
TEST(Elm327ProtocolTest, AnOverlongLineIsFlaggedAndTheNextIsWhole)
{
    elm327_reader r;
    const std::string longline(ELM327_LINE_MAX + 40, 'A');
    const auto ev = events_of(r, longline + "\rOK\r");
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0], std::string(ELM327_LINE_MAX, 'A') + "<overflow>");
    EXPECT_EQ(ev[1], "OK");

    elm327_reader q;
    for (const char c : longline + "\r") {
        if (elm327_take(q, static_cast<uint8_t>(c)).line) {
            EXPECT_EQ(elm327_classify(q, false, nullptr),
                      elm327_line::malformed);
        }
    }
}

/* The same reply gives the same messages however its bytes arrive: in
 * one piece, a byte at a time, and split in two at every position. */
TEST(Elm327ProtocolTest, AReplyReassemblesTheSameInAnyChunking)
{
    const Reassembled whole = reassemble(kInterleaved, {});
    EXPECT_EQ(whole.messages, kInterleavedMessages);
    EXPECT_EQ(whole.lines, 6u);
    EXPECT_EQ(whole.prompts, 1u);

    const Reassembled bytewise =
        reassemble(kInterleaved, std::vector<size_t>(kInterleaved.size(), 1));
    EXPECT_EQ(bytewise.messages, kInterleavedMessages);

    for (size_t split = 1; split < kInterleaved.size(); ++split) {
        const Reassembled two = reassemble(kInterleaved, {split});
        EXPECT_EQ(two.messages, kInterleavedMessages) << "split at " << split;
        EXPECT_EQ(two.prompts, 1u) << "split at " << split;
    }
}

/* Spaces off, linefeeds on (the OBDX's default), upper or lower case:
 * the same messages. */
TEST(Elm327ProtocolTest, SpacesLinefeedsAndCaseDontMatter)
{
    std::string compact;
    for (const char c : kInterleaved) {
        if (c == ' ') {
            continue;
        }
        compact += c;
        if (c == '\r') {
            compact += '\n';
        }
    }
    EXPECT_EQ(reassemble(compact, {}).messages, kInterleavedMessages);

    std::string lower = kInterleaved;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    EXPECT_EQ(reassemble(lower, {}).messages, kInterleavedMessages);
}

/* ---- Line kinds ----------------------------------------------------- */

/* Every word the three manuals list, as an ELM327 prints it and as a
 * clone might under AT S0 or in lower case, and the lines a monitor
 * marks (a flow control's FC:, an error beside a frame's bytes), which
 * aren't frames. */
TEST(Elm327ProtocolTest, EveryWordHasItsKind)
{
    struct Case {
        const char* line;
        elm327_line kind;
    };
    const Case cases[] = {
        {"OK", elm327_line::ok},
        {"ok", elm327_line::ok},
        {"NO DATA", elm327_line::no_data},
        {"NODATA", elm327_line::no_data},
        {"No Data", elm327_line::no_data},
        {"CAN ERROR", elm327_line::no_bus},
        {"BUS ERROR", elm327_line::no_bus},
        {"BUS BUSY", elm327_line::no_bus},
        {"FB ERROR", elm327_line::no_bus},
        {"UNABLE TO CONNECT", elm327_line::no_bus},
        {"ERR94", elm327_line::no_bus},
        {"ERR 71", elm327_line::no_bus},
        {"BUFFER FULL", elm327_line::buffer_full},
        {"STOPPED", elm327_line::stopped},
        {"?", elm327_line::refused},
        {"DATA ERROR", elm327_line::read_error},
        {"<DATA ERROR", elm327_line::read_error},
        {"<RX ERROR", elm327_line::read_error},
        {"7E8 03 41 0D <DATA ERROR", elm327_line::read_error},
        {"7E8 02 01 0C <RX ERROR", elm327_line::read_error},
        /* A monitor's flow control: not a frame */
        {"7E8 FC: 30 00 00", elm327_line::text},
        {"7E8FC:300000", elm327_line::text},
        {"FC: 30 00 00", elm327_line::text},
        {"FC RX TIMEOUT", elm327_line::read_error},
        {"OUT OF MEMORY", elm327_line::read_error},
        {"UART RX OVERFLOW", elm327_line::read_error},
        {"LV RESET", elm327_line::reset},
        {"ELM327 v1.4b", elm327_line::reset},
        {"ELM327 V1.4", elm327_line::reset},
        {"elm327 v2.1", elm327_line::reset},
        {"ACT ALERT", elm327_line::alert},
        {"LP ALERT", elm327_line::alert},
        {"SEARCHING...", elm327_line::searching},
        {"12.6V", elm327_line::text},
        {"STN2255 v5.10.3", elm327_line::text},
        {"BUS INIT: ...OK", elm327_line::text},
        {"ERROR", elm327_line::text},
        {"6", elm327_line::malformed},
        {"A6", elm327_line::malformed},
        {"7E8", elm327_line::malformed},
    };
    for (const Case& k : cases) {
        EXPECT_EQ(kind_of(k.line), k.kind) << k.line;
    }
}

/* ---- Frame shapes --------------------------------------------------- */

/* 11-bit: a three-digit ID, or four with a leading 0, told apart by the
 * line's length; 29-bit: eight digits. One to eight data bytes, an ID in
 * the mode's range, every character hex. */
TEST(Elm327ProtocolTest, FrameLinesFollowTheAddressingMode)
{
    elm327_can_line f = frame_of("7E8 06 41 00 BE 3F A8 13");
    EXPECT_EQ(f.id, 0x7E8u);
    ASSERT_EQ(f.len, 7u);
    EXPECT_EQ(f.data[0], 0x06u);
    EXPECT_EQ(f.data[6], 0x13u);

    f = frame_of("7E8064100BE3FA813");
    EXPECT_EQ(f.id, 0x7E8u);
    EXPECT_EQ(f.len, 7u);

    f = frame_of("07E8064100BE3FA813");
    EXPECT_EQ(f.id, 0x7E8u) << "a two-byte ID";
    EXPECT_EQ(f.len, 7u);
    f = frame_of("07 E8 06 41 00 BE 3F A8 13");
    EXPECT_EQ(f.id, 0x7E8u);

    f = frame_of("7E8 03 41 0D 32 00 00 00 00");
    EXPECT_EQ(f.len, 8u) << "padding is kept for the reassembly to ignore";

    f = frame_of("18DAF110 03 41 0D 32", true);
    EXPECT_EQ(f.id, 0x18DAF110u);
    EXPECT_EQ(f.len, 4u);

    EXPECT_EQ(kind_of("17E8064100BE3FA813"), elm327_line::malformed)
        << "four digits without the leading 0";
    EXPECT_EQ(kind_of("FE80641"), elm327_line::malformed) << "past 0x7FF";
    EXPECT_EQ(kind_of("7E8 06 41 00 BE 3F A8 13 00 00"), elm327_line::malformed)
        << "nine data bytes";
    EXPECT_EQ(kind_of("18DAF110", true), elm327_line::malformed) << "no data";
    EXPECT_EQ(kind_of("18DAF11003410D3", true), elm327_line::malformed)
        << "half a byte";
    EXPECT_EQ(kind_of("3FDAF11003410D32", true), elm327_line::malformed)
        << "past 29 bits";
    EXPECT_EQ(kind_of("7E8 03 41 0D 32", true), elm327_line::malformed)
        << "an 11-bit line in 29-bit mode";
}

/* ---- ISO-TP --------------------------------------------------------- */

namespace {

/* A Single Frame's message points into its frame, so the frame is kept
 * here until the next take, as elm327_message's contract asks */
elm327_isotp_result take(elm327_isotp& iso, const char* line, elm327_message* m,
                         bool ext29 = false)
{
    static elm327_can_line f;
    f = frame_of(line, ext29);
    return elm327_isotp_take(iso, f, m);
}

} // namespace

/* A Single Frame is its message: the PCI byte's length, padding ignored.
 * A length of 0 or past 7, or one the frame doesn't hold, is malformed. */
TEST(Elm327ProtocolTest, SingleFramesTakeThePciLength)
{
    static elm327_isotp iso;
    iso = elm327_isotp();
    elm327_message m;
    ASSERT_EQ(take(iso, "7E8 03 41 0D 32 AA AA AA AA", &m),
              elm327_isotp_result::complete);
    EXPECT_EQ(message_text(m), "7E8: 41 0D 32");

    EXPECT_EQ(take(iso, "7E8 00 41 0D", &m), elm327_isotp_result::malformed);
    EXPECT_EQ(take(iso, "7E8 08 41 0D 32 00 00 00 00", &m),
              elm327_isotp_result::malformed);
    EXPECT_EQ(take(iso, "7E8 05 41 0D 32", &m), elm327_isotp_result::malformed)
        << "says five, holds three";
    EXPECT_EQ(iso.malformed, 3u);
    EXPECT_EQ(iso.dropped, 0u);
}

/* A message across the sequence's wrap (21 ... 2F, 20, 21, ...): the
 * longest a classic CAN reply carries, 4095 bytes, whole. */
TEST(Elm327ProtocolTest, ALongMessageCrossesTheSequenceWrap)
{
    static elm327_isotp iso;
    iso = elm327_isotp();
    std::vector<uint8_t> want(4095);
    for (size_t i = 0; i < want.size(); ++i) {
        want[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    elm327_can_line f;
    f.id = 0x7E8;
    f.len = 8;
    f.data[0] = 0x1F;
    f.data[1] = 0xFF;
    memcpy(f.data + 2, want.data(), 6);
    elm327_message m;
    ASSERT_EQ(elm327_isotp_take(iso, f, &m), elm327_isotp_result::pending);
    size_t have = 6;
    uint8_t seq = 1;
    elm327_isotp_result last = elm327_isotp_result::pending;
    while (have < want.size()) {
        const size_t take_n = std::min<size_t>(7, want.size() - have);
        f.data[0] = static_cast<uint8_t>(0x20 | seq);
        memset(f.data + 1, 0xAA, 7);
        memcpy(f.data + 1, want.data() + have, take_n);
        last = elm327_isotp_take(iso, f, &m);
        have += take_n;
        seq = static_cast<uint8_t>((seq + 1) & 0x0F);
        if (have < want.size()) {
            ASSERT_EQ(last, elm327_isotp_result::pending) << have;
        }
    }
    ASSERT_EQ(last, elm327_isotp_result::complete);
    ASSERT_EQ(m.len, 4095u);
    EXPECT_EQ(memcmp(m.data, want.data(), want.size()), 0);
    EXPECT_EQ(iso.dropped, 0u);
}

/* A message that can't finish is dropped, counted once: a sequence gap, a
 * repeated frame, a Consecutive Frame too short for what's left, a new
 * First Frame or a Single Frame on the same ID, and the reply's end. */
TEST(Elm327ProtocolTest, BrokenMessagesAreDroppedAndCounted)
{
    static elm327_isotp iso;
    iso = elm327_isotp();
    elm327_message m;
    const char* ff = "7E8 10 13 49 04 01 35 36 30";

    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending);
    EXPECT_EQ(take(iso, "7E8 22 00 00 00 00 00 00 31", &m),
              elm327_isotp_result::dropped)
        << "21 skipped";
    EXPECT_EQ(iso.dropped, 1u);

    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending);
    ASSERT_EQ(take(iso, "7E8 21 32 38 39 34 39 41 43", &m),
              elm327_isotp_result::pending);
    EXPECT_EQ(take(iso, "7E8 21 32 38 39 34 39 41 43", &m),
              elm327_isotp_result::dropped)
        << "21 again";
    EXPECT_EQ(iso.dropped, 2u);

    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending);
    EXPECT_EQ(take(iso, "7E8 21 32 38 39", &m), elm327_isotp_result::dropped)
        << "three bytes where seven are due";
    EXPECT_EQ(iso.dropped, 3u);

    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending);
    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending)
        << "a new First Frame starts over";
    EXPECT_EQ(iso.dropped, 4u);
    ASSERT_EQ(take(iso, "7E8 03 41 0D 32", &m), elm327_isotp_result::complete)
        << "a Single Frame is whole whatever came before";
    EXPECT_EQ(iso.dropped, 5u);

    ASSERT_EQ(take(iso, ff, &m), elm327_isotp_result::pending);
    EXPECT_EQ(elm327_isotp_end_reply(iso), 1u) << "the prompt ends it";
    EXPECT_EQ(iso.dropped, 6u);
    EXPECT_EQ(take(iso, "7E8 21 32 38 39 34 39 41 43", &m),
              elm327_isotp_result::malformed)
        << "nothing under way";
}

/* What no message can use: a First Frame shorter than eight bytes or
 * announcing fewer than eight, a PCI type past Flow Control. Flow
 * Control frames are ignored. */
TEST(Elm327ProtocolTest, FirstFramesAndPciTypesAreChecked)
{
    static elm327_isotp iso;
    iso = elm327_isotp();
    elm327_message m;
    EXPECT_EQ(take(iso, "7E8 10 13 49 04 01", &m),
              elm327_isotp_result::malformed);
    EXPECT_EQ(take(iso, "7E8 10 07 49 04 01 35 36 30", &m),
              elm327_isotp_result::malformed)
        << "seven fits a Single Frame";
    EXPECT_EQ(take(iso, "7E8 10 00 00 00 10 00 49 04", &m),
              elm327_isotp_result::malformed)
        << "the 32-bit length escape";
    EXPECT_EQ(take(iso, "7E8 40 01 02", &m), elm327_isotp_result::malformed);
    EXPECT_EQ(take(iso, "7E0 30 00 00", &m), elm327_isotp_result::ignored);
    EXPECT_EQ(iso.malformed, 4u);
}

/* With every slot under way, a new message takes the oldest's place. */
TEST(Elm327ProtocolTest, TheOldestMessageGivesWayWhenEverySlotIsTaken)
{
    static elm327_isotp iso;
    iso = elm327_isotp();
    elm327_message m;
    elm327_can_line f;
    f.len = 8;
    f.data[0] = 0x10;
    f.data[1] = 0x0D;
    for (uint32_t i = 0; i <= ELM327_ISOTP_SLOTS; ++i) {
        f.id = 0x700 + i;
        ASSERT_EQ(elm327_isotp_take(iso, f, &m), elm327_isotp_result::pending);
    }
    EXPECT_EQ(iso.dropped, 1u);
    EXPECT_EQ(elm327_isotp_find(iso, 0x700), nullptr) << "the oldest went";
    EXPECT_NE(elm327_isotp_find(iso, 0x700 + ELM327_ISOTP_SLOTS), nullptr);
}

/* A negative reply and a pending one arrive as Single Frames and reach the
 * request engine as the J2534 transport's would: 7F 22 78 holds the
 * request, 7F 22 31 completes it. */
TEST(Elm327ProtocolTest, NegativeAndPendingRepliesReachTheEngine)
{
    ecuio_request_engine e;
    ecuio_engine_init(&e, 1);
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    ASSERT_EQ(ecuio_engine_add(&e, 0, did, 3), ecuio_engine_add_result::added);
    ecuio_engine_sent(&e, 0, ecuio_engine_due(&e, 0, 0), true, 0);

    static elm327_isotp iso;
    iso = elm327_isotp();
    elm327_message m;
    uint8_t nrc = 0;
    ASSERT_EQ(take(iso, "7E8 03 7F 22 78 AA AA AA AA", &m),
              elm327_isotp_result::complete);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, m.data, m.len, 1000, &nrc),
              ecuio_reply::pending);
    EXPECT_EQ(nrc, 0x78u);

    ASSERT_EQ(take(iso, "7E8037F2231", &m), elm327_isotp_result::complete);
    EXPECT_EQ(ecuio_engine_on_frame(&e, 0, m.data, m.len, 2000, &nrc),
              ecuio_reply::negative);
    EXPECT_EQ(nrc, 0x31u);
}

/* ---- Values --------------------------------------------------------- */

TEST(Elm327ProtocolTest, VoltagesReadInMillivoltsOrNotAtAll)
{
    struct Case {
        const char* text;
        bool ok;
        uint32_t mv;
    };
    const Case cases[] = {
        {"12.6V", true, 12600},   {"14.5v", true, 14500},
        {"12.34", true, 12340},   {"12.345", true, 12345},
        {" 13.8V ", true, 13800}, {"0.0V", true, 0},
        {"12V", true, 12000},     {"9", true, 9000},
        {"--.--", false, 0},      {"--.-V", false, 0},
        {"", false, 0},           {"V", false, 0},
        {".5V", false, 0},        {"12.V", false, 0},
        {"12.3456", false, 0},    {"1000.0V", false, 0},
        {"12,6V", false, 0},      {"12.6 V", false, 0},
        {"12.6VV", false, 0},     {"?", false, 0},
    };
    for (const Case& k : cases) {
        uint32_t mv = 777;
        EXPECT_EQ(elm327_parse_millivolts(k.text, strlen(k.text), &mv), k.ok)
            << '"' << k.text << '"';
        EXPECT_EQ(mv, k.ok ? k.mv : 777u) << '"' << k.text << '"';
    }
}

/* AT PPS lines, as the datasheet prints them: a parameter's value and
 * whether it's on, from any entry of the line, spaced or not */
TEST(Elm327ProtocolTest, ParameterSummaryLinesGiveEachValueAndState)
{
    const char line[] = "28:FF F  29:FF F  2A:38 F  2B:02 F";
    uint8_t value = 0;
    bool on = true;
    ASSERT_TRUE(elm327_pps_value(line, strlen(line), 0x2A, &value, &on));
    EXPECT_EQ(value, 0x38);
    EXPECT_FALSE(on);
    ASSERT_TRUE(elm327_pps_value(line, strlen(line), 0x28, &value, &on));
    EXPECT_EQ(value, 0xFF);
    ASSERT_TRUE(elm327_pps_value(line, strlen(line), 0x2B, &value, &on));
    EXPECT_EQ(value, 0x02);

    const char tight[] = "28:FFF29:FFF2a:00n2B:02F";
    ASSERT_TRUE(elm327_pps_value(tight, strlen(tight), 0x2A, &value, &on));
    EXPECT_EQ(value, 0x00);
    EXPECT_TRUE(on) << "N in either case is on";

    for (const char* bad : {"00:FF F  01:00 N", "2A:04", "2A:04 X", "2A=04 N",
                            "2A:0G N", "OK", "?", ""}) {
        value = 0x55;
        EXPECT_FALSE(elm327_pps_value(bad, strlen(bad), 0x2A, &value, &on))
            << '"' << bad << '"';
        EXPECT_EQ(value, 0x55) << '"' << bad << '"';
    }
}

TEST(Elm327ProtocolTest, VersionStringsAndEchoesAreRecognized)
{
    for (const char* v :
         {"ELM327 v1.4b", "ELM327 V1.4", "elm327 v2.1", "  ELM327 v1.5"}) {
        EXPECT_TRUE(elm327_is_version(v, strlen(v))) << v;
    }
    for (const char* v : {"ELM32", "OBDLink MX+", "STN2255 v5.10.3", ""}) {
        EXPECT_FALSE(elm327_is_version(v, strlen(v))) << v;
    }
    for (const char* v :
         {"STN2255 v5.10.3", "STN2232 v5.10.3", "stn1110 v4.0"}) {
        EXPECT_TRUE(elm327_is_stn(v, strlen(v))) << v;
    }
    for (const char* v : {"?", "STN", "STNX v1", "ELM327 v1.4b"}) {
        EXPECT_FALSE(elm327_is_stn(v, strlen(v))) << v;
    }

    const char* echo = "AT E0";
    EXPECT_TRUE(elm327_is_echo(echo, strlen(echo), "ATE0"));
    EXPECT_TRUE(elm327_is_echo("atz", 3, "ATZ"));
    EXPECT_FALSE(elm327_is_echo("OK", 2, "ATE0"));
    EXPECT_FALSE(elm327_is_echo("ATE", 3, "ATE0"));
}

/* ---- Commands ------------------------------------------------------- */

namespace {

/* A numeric def keyed 41 <pid>, its field start_byte to start_byte +
 * bytes */
ecuio_signal_def_t pid_def(uint8_t pid, uint16_t start_byte, uint8_t bytes)
{
    ecuio_signal_def_t d{};
    d.message_id = 0x7E8;
    d.mux_length = 2;
    d.mux_key = 0x4100u | pid;
    d.kind = ECUIO_VALUE_UINT;
    d.byte_order = ECUIO_BYTE_ORDER_BE;
    d.start_byte = start_byte;
    d.bit_length = static_cast<uint8_t>(8 * bytes);
    d.scale = 1.0;
    return d;
}

ecuio_signal_set_t set_of(const ecuio_signal_def_t* defs, uint32_t n)
{
    ecuio_signal_set_t set{};
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = n;
    set.defs = defs;
    return set;
}

} // namespace

/* The PIDs given the count digit: the bitmaps, and a PID whose every def
 * ends within a Single Frame's seven bytes; a text field to the end, or
 * any def past the seventh byte, keeps the digit off */
TEST(Elm327ProtocolTest, TheCountDigitGoesToPidsWhoseDefsFitOneFrame)
{
    ecuio_signal_def_t defs[7] = {
        pid_def(0x0C, 2, 2), /* rpm: ends at 4 */
        pid_def(0x42, 2, 2),
        pid_def(0x7F, 3, 4), /* the first field of a long reply: ends at 7 */
        pid_def(0x81, 3, 4), /* fits, but the next def doesn't */
        pid_def(0x81, 7, 4), /* ends at 11 */
        pid_def(0x01, 2, 1),
        pid_def(0x02, 2, 1),
    };
    defs[5].kind = ECUIO_VALUE_TEXT; /* to the end of the payload */
    defs[5].bit_length = 0;
    defs[6].kind = ECUIO_VALUE_HEX; /* 2 to 7 */
    defs[6].bit_length = 0;
    defs[6].byte_length = 5;
    const ecuio_signal_set_t set = set_of(defs, 7);
    elm327_one_frame_pids one;
    elm327_one_frame_pids_for(&set, &one);
    EXPECT_TRUE(elm327_one_frame(one, 0x0C));
    EXPECT_TRUE(elm327_one_frame(one, 0x42));
    EXPECT_TRUE(elm327_one_frame(one, 0x7F))
        << "the defs are all the rule sees; the transport learns the rest";
    EXPECT_FALSE(elm327_one_frame(one, 0x81));
    EXPECT_FALSE(elm327_one_frame(one, 0x01));
    EXPECT_TRUE(elm327_one_frame(one, 0x02));
    EXPECT_FALSE(elm327_one_frame(one, 0x0D)) << "no def: length unknown";
    for (unsigned pid = 0; pid <= 0xE0; pid += 0x20) {
        EXPECT_TRUE(elm327_one_frame(one, static_cast<uint8_t>(pid))) << pid;
    }

    /* A def keyed another way enrols nothing here */
    ecuio_signal_def_t other = pid_def(0x05, 9, 1);
    other.mux_offset = 1;
    const ecuio_signal_set_t odd = set_of(&other, 1);
    elm327_one_frame_pids_for(&odd, &one);
    EXPECT_FALSE(elm327_one_frame(one, 0x05));
    elm327_one_frame_pids_for(nullptr, &one);
    EXPECT_TRUE(elm327_one_frame(one, 0x00));
    EXPECT_FALSE(elm327_one_frame(one, 0x0C));

    elm327_one_frame_set(one, 0x0C, true);
    EXPECT_TRUE(elm327_one_frame(one, 0x0C));
    elm327_one_frame_set(one, 0x0C, false);
    EXPECT_FALSE(elm327_one_frame(one, 0x0C));
    EXPECT_TRUE(elm327_one_frame(one, 0x00)) << "a neighbor's bit stays";
}

TEST(Elm327ProtocolTest, RequestsCarryTheCountDigitOnlyWhereOneReplyIsCertain)
{
    elm327_one_frame_pids one;
    elm327_one_frame_set(one, 0x0C, true);
    const uint8_t rpm[2] = {0x01, 0x0C};
    const uint8_t speed[2] = {0x01, 0x0D};
    const uint8_t vin[2] = {0x09, 0x02};
    const uint8_t did[3] = {0x22, 0x1E, 0x1C};
    const uint8_t did_0c[3] = {0x01, 0x0C, 0x00};
    EXPECT_EQ(elm327_response_count(rpm, 2, 0x7E0, one), 1u);
    EXPECT_EQ(elm327_response_count(rpm, 2, 0x18DA10F1, one), 1u);
    EXPECT_EQ(elm327_response_count(rpm, 2, 0x7DF, one), 0u) << "functional";
    EXPECT_EQ(elm327_response_count(rpm, 2, 0x18DB33F1, one), 0u)
        << "functional";
    EXPECT_EQ(elm327_response_count(speed, 2, 0x7E0, one), 0u)
        << "not in the set";
    EXPECT_EQ(elm327_response_count(vin, 2, 0x7E0, one), 0u);
    EXPECT_EQ(elm327_response_count(did, 3, 0x7E0, one), 0u);
    EXPECT_EQ(elm327_response_count(did_0c, 3, 0x7E0, one), 0u);

    char out[32];
    EXPECT_EQ(elm327_format_request(rpm, 2, 1, out, sizeof out), 6u);
    EXPECT_STREQ(out, "010C1\r");
    const uint8_t vin_did[3] = {0x22, 0xF1, 0x90};
    EXPECT_EQ(elm327_format_request(vin_did, 3, 0, out, sizeof out), 7u);
    EXPECT_STREQ(out, "22F190\r");
    EXPECT_EQ(elm327_format_request(rpm, 2, 15, out, sizeof out), 6u);
    EXPECT_STREQ(out, "010CF\r");

    EXPECT_EQ(elm327_format_request(rpm, 2, 16, out, sizeof out), 0u);
    EXPECT_EQ(elm327_format_request(rpm, 0, 1, out, sizeof out), 0u);
    EXPECT_EQ(elm327_format_request(rpm, 2, 1, out, 6), 0u)
        << "no room for NUL";
    EXPECT_EQ(elm327_format_request(rpm, 2, 1, out, 7), 6u);
}

TEST(Elm327ProtocolTest, HeadersAndReceiveFiltersFollowTheMode)
{
    char cmd[2][24];
    ASSERT_EQ(elm327_format_header(0x7E0, false, cmd), 1u);
    EXPECT_STREQ(cmd[0], "ATSH7E0\r");
    ASSERT_EQ(elm327_format_header(0x18DA10F1, true, cmd), 2u);
    EXPECT_STREQ(cmd[0], "ATCP18\r");
    EXPECT_STREQ(cmd[1], "ATSHDA10F1\r");

    const uint32_t one[1] = {0x7E8};
    ASSERT_EQ(elm327_format_receive_filter(one, 1, false, cmd), 1u);
    EXPECT_STREQ(cmd[0], "ATCRA7E8\r");
    const uint32_t one29[1] = {0x18DAF110};
    ASSERT_EQ(elm327_format_receive_filter(one29, 1, true, cmd), 1u);
    EXPECT_STREQ(cmd[0], "ATCRA18DAF110\r");

    const uint32_t two[2] = {0x7E8, 0x7E9};
    ASSERT_EQ(elm327_format_receive_filter(two, 2, false, cmd), 2u);
    EXPECT_STREQ(cmd[0], "ATCF7E8\r");
    EXPECT_STREQ(cmd[1], "ATCM7FE\r");

    /* Whatever the IDs, the filter and mask pass every one of them. */
    const uint32_t spread[3] = {0x7E8, 0x7EB, 0x6F1};
    ASSERT_EQ(elm327_format_receive_filter(spread, 3, false, cmd), 2u);
    const uint32_t filter =
        static_cast<uint32_t>(strtoul(cmd[0] + 4, nullptr, 16));
    const uint32_t mask =
        static_cast<uint32_t>(strtoul(cmd[1] + 4, nullptr, 16));
    for (const uint32_t id : spread) {
        EXPECT_EQ(id & mask, filter) << std::hex << id;
    }
    EXPECT_EQ(elm327_format_receive_filter(spread, 0, false, cmd), 0u);
}

TEST(Elm327ProtocolTest, TheProtocolFollowsTheModeAndTheRate)
{
    EXPECT_EQ(elm327_protocol_for(false, 500000), '6');
    EXPECT_EQ(elm327_protocol_for(true, 500000), '7');
    EXPECT_EQ(elm327_protocol_for(false, 250000), '8');
    EXPECT_EQ(elm327_protocol_for(true, 250000), '9');
    EXPECT_EQ(elm327_protocol_for(false, 125000), 0);
    EXPECT_EQ(elm327_protocol_for(false, 0), 0);
}

/* A WiFi adapter's address (ADR-0026's appendix "WiFi over TCP"): four
 * octets, a colon and a port, nothing else; a serial port's name never
 * reads as one */
TEST(Elm327TcpTest, AddressesAreADottedIpv4AndAPort)
{
    struct Good {
        const char* name;
        uint32_t ip;
        uint16_t port;
    };
    const Good good[] = {
        {"192.168.4.1:23", 0xC0A80401u, 23},
        {"192.168.0.10:35000", 0xC0A8000Au, 35000},
        {"127.0.0.1:1", 0x7F000001u, 1},
        {"0.0.0.0:65535", 0u, 65535},
        {"255.255.255.255:80", 0xFFFFFFFFu, 80},
    };
    for (const Good& g : good) {
        uint32_t ip = 1;
        uint16_t port = 0;
        EXPECT_TRUE(elm327_tcp_address(g.name, strlen(g.name), &ip, &port))
            << g.name;
        EXPECT_EQ(ip, g.ip) << g.name;
        EXPECT_EQ(port, g.port) << g.name;
    }
    const char* const bad[] = {
        "192.168.4.1",       /* no port */
        "192.168.4.1:",      /* an empty port */
        "192.168.4.1:0",     /* port 0 */
        "192.168.4.1:65536", /* past 65535 */
        "192.168.4.1:023",   /* a leading zero */
        "192.168.4.1:2a",    /* not digits */
        "192.168.4.1:23:1",  /* two colons */
        "192.168.4:23",      /* three octets */
        "192.168.4.1.5:23",  /* five */
        "192.168.4.256:23",  /* an octet past 255 */
        "192.168.04.1:23",   /* a leading zero in an octet */
        "192.168..1:23",     /* an empty octet */
        ".192.168.4.1:23",   /* a stray dot */
        "192.168.4.1.:23",
        "-1.168.4.1:23",
        " 192.168.4.1:23", /* a space */
        "obdx.local:23",   /* a host name */
        "[::1]:23",        /* IPv6 */
        "::1",
        "COM5", /* serial ports */
        "/dev/ttyUSB0",
        "",
    };
    for (const char* b : bad) {
        EXPECT_FALSE(elm327_tcp_address(b, strlen(b), nullptr, nullptr)) << b;
    }
    /* The length bounds the name, not a NUL: a port cut short is refused */
    EXPECT_FALSE(elm327_tcp_address("192.168.4.1:23", 12, nullptr, nullptr));
    EXPECT_TRUE(elm327_tcp_address("192.168.4.1:2345", 14, nullptr, nullptr));
}

/* A failed connect says what happened in words: the bench's reconnect
 * after the adapter's WiFi went away read "Windows sockets error 10053";
 * a code with no words of its own still names itself */
TEST(Elm327TcpTest, ConnectFailuresAreWorded)
{
#ifdef _WIN32
    const int refused = 10061, unreachable = 10051, timed_out = 10060,
              aborted = 10053, reset = 10054, other = 10013;
#else
    const int refused = ECONNREFUSED, unreachable = ENETUNREACH,
              timed_out = ETIMEDOUT, aborted = ECONNABORTED,
              reset = ECONNRESET, other = EACCES;
#endif
    char why[160];
    const auto words = [&](int e) {
        why[0] = '\0';
        elm327_tcp_describe(e, why, sizeof(why));
        return std::string(why);
    };
    EXPECT_EQ(words(refused), "the connection was refused");
    EXPECT_EQ(words(unreachable), "the network is unreachable");
    EXPECT_EQ(words(timed_out),
              "no answer within 2 s (is this computer on the adapter's "
              "WiFi?)");
    EXPECT_EQ(words(aborted), "the connection was cut off on this "
                              "computer's side (did it leave the adapter's "
                              "WiFi?)");
    EXPECT_EQ(words(reset), "the adapter reset the connection");
    EXPECT_EQ(words(other).rfind("the connection failed (", 0), 0u)
        << words(other);
}
