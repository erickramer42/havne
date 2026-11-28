// File/layer: tests/test_sim_memory_faults.cpp | C++17, Qt-free, every
//             platform
// Testing: UNIT: SimMemoryTest, FakeMemoryTest (test_obd2)
//
// The simulated memory's own faults (core/src/sim_memory.h, the URI's
// mem=): the grammar and its refusals, each fault's period and what it
// does to a read and to an upload, and the key through the unbound sim
// and the fakes' knob.

#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "sim_memory_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace sim_memory_test;

const char kGrammar[] =
    " isn't <fault>:<n>: short, flip, relock, seq, refuse or ident with n "
    "from 1 to 10000, volatile with a hex offset into the region (0 to FFFF) "
    "or block with a hex length from 1 to FFF";

/* What mem=text parses to, the parse expected to pass */
ecuio_sim_memory_faults faults_of(const std::string& text)
{
    ecuio_sim_memory_faults f;
    char why[256] = {};
    EXPECT_TRUE(
        ecuio_sim_parse_memory(text.data(), text.size(), &f, why, sizeof why))
        << text << ": " << why;
    return f;
}

std::string refusal_of(const std::string& text)
{
    ecuio_sim_memory_faults f;
    char why[256] = {};
    EXPECT_FALSE(
        ecuio_sim_parse_memory(text.data(), text.size(), &f, why, sizeof why))
        << text;
    return why;
}

/* The bits two replies of one length differ in, and where the first is */
int bits_apart(const Bytes& a, const Bytes& b, size_t* at)
{
    int bits = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        for (int k = 0; k < 8; ++k) {
            if (((a[i] ^ b[i]) >> k) & 1) {
                if (bits++ == 0) {
                    *at = i;
                }
            }
        }
    }
    return bits;
}

TEST(SimMemoryTest, TheMemKeyNamesEachFaultAndItsNumber)
{
    const ecuio_sim_memory_faults f = faults_of(
        "short:2,flip:3,relock:4,seq:5,refuse:6,ident:7,volatile:1F,block:82");
    EXPECT_TRUE(f.given);
    EXPECT_EQ(f.cut, 2u);
    EXPECT_EQ(f.flip, 3u);
    EXPECT_EQ(f.relock, 4u);
    EXPECT_EQ(f.seq, 5u);
    EXPECT_EQ(f.refuse, 6u);
    EXPECT_EQ(f.ident, 7u);
    EXPECT_TRUE(f.moving);
    EXPECT_EQ(f.moving_at, 0x1Fu);
    EXPECT_EQ(f.block, 0x82);

    const ecuio_sim_memory_faults edge =
        faults_of("flip:10000,volatile:ffff,block:FFF,short:1");
    EXPECT_EQ(edge.flip, 10000u);
    EXPECT_EQ(edge.moving_at, 0xFFFFu);
    EXPECT_EQ(edge.block, 0xFFF);
    EXPECT_EQ(edge.cut, 1u);
    EXPECT_EQ(faults_of("volatile:0").moving_at, 0u);
    EXPECT_TRUE(faults_of("volatile:0").moving);
    EXPECT_EQ(faults_of("flip:2,flip:9").flip, 9u) << "a later item wins";
    EXPECT_FALSE(ecuio_sim_memory_faults{}.given);
    EXPECT_EQ(ecuio_sim_memory_faults{}.block, 0x402);

    /* A second mem= key adds to the first */
    ecuio_sim_config c;
    char why[256] = {};
    ASSERT_EQ(ecuio_sim_parse_uri("mock://x?mem=flip:2,short:3&mem=flip:5", &c,
                                  why, sizeof why),
              ecuio_sim_uri_result::ok)
        << why;
    EXPECT_EQ(c.memory.flip, 5u);
    EXPECT_EQ(c.memory.cut, 3u);
    ASSERT_EQ(ecuio_sim_parse_uri("mock://x", &c, why, sizeof why),
              ecuio_sim_uri_result::ok);
    EXPECT_FALSE(c.memory.given);
}

TEST(SimMemoryTest, AMemRefusalNamesItsItemAndTheGrammar)
{
    for (const char* bad :
         {"flip", "flip:", "flip:0", "flip:10001", "flip:2x", "flip:-1",
          "flip:2@7E8", "bogus:1", ":3", "block:0", "block:1000",
          "block:", "volatile:10000", "volatile:xy",
          "volatile:", "short:99999999999", "Flip:2"}) {
        EXPECT_EQ(refusal_of(bad), std::string("mem: '") + bad + "'" + kGrammar)
            << bad;
    }
    EXPECT_EQ(refusal_of("flip:2,,short:3"), std::string("mem: ''") + kGrammar);
    EXPECT_EQ(refusal_of("flip:2,bogus:1"),
              std::string("mem: 'bogus:1'") + kGrammar)
        << "the item, not the whole key";
    EXPECT_EQ(refusal_of(""), "mem: names none");
    EXPECT_EQ(refusal_of("flip:2,"), "mem: names none");

    /* Through the URI: the key's own words, and the key is mock://'s */
    ecuio_sim_config c;
    char why[256] = {};
    EXPECT_EQ(ecuio_sim_parse_uri("mock://x?mem=flip:0", &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
    EXPECT_EQ(std::string(why), std::string("mem: 'flip:0'") + kGrammar);
#ifdef _WIN32
    const char replay[] = "replay://C:/rec/frames.csv?mem=flip:2";
#else
    const char replay[] = "replay:///rec/frames.csv?mem=flip:2";
#endif
    EXPECT_EQ(ecuio_sim_parse_uri(replay, &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
    EXPECT_NE(std::string(why).find("no query key 'mem' for replay://"),
              std::string::npos)
        << why;
    EXPECT_EQ(ecuio_sim_parse_uri("mock://x?bogus=1", &c, why, sizeof why),
              ecuio_sim_uri_result::bad);
    EXPECT_NE(std::string(why).find(
                  "sa_delay, gwopt, mem, kernel, gated, vin, faults"),
              std::string::npos)
        << why;
}

TEST(SimMemoryTest, ShortCutsEveryNthDataReplyByOneByte)
{
    Ecu e;
    e.m.faults.cut = 3;
    const Bytes whole = data_of(kBase + 5, 40, e.m.vin);
    for (int round = 0; round < 2; ++round) {
        EXPECT_EQ(e.ask(read_of(kBase + 5, 40)), whole);
        EXPECT_EQ(e.ask(read_of(kBase + 5, 40)), whole);
        EXPECT_EQ(e.ask(read_of(kBase + 5, 40)),
                  Bytes(whole.begin(), whole.end() - 1))
            << "the third, nothing padded and nothing else changed";
    }
    EXPECT_EQ(e.ask(read_of(kBase, 16, 1, 1)), nrc(0x23, 0x31));
    EXPECT_EQ(e.m.replies, 6u) << "a refused read is no data reply";

    /* A block counts too, and the upload moves on past the lost byte */
    Ecu u;
    u.m.faults.cut = 2;
    u.ask(upload_of(kBase, 3000));
    EXPECT_EQ(u.ask({0x36, 0x01}).size(), 0x402u);
    EXPECT_EQ(u.ask({0x36, 0x02}).size(), 0x401u);
    EXPECT_EQ(u.m.left, 3000u - 2048u);
    Ecu one;
    one.m.faults.cut = 1;
    EXPECT_EQ(one.ask(read_of(kBase, 1)), (Bytes{0x63}));
}

TEST(SimMemoryTest, FlipChangesOneBitOfEveryNthDataReplyAtAPlaceByTheCount)
{
    Ecu e;
    e.m.faults.flip = 2;
    const Bytes whole = data_of(kBase, 50, e.m.vin);
    for (uint32_t k = 1; k <= 8; ++k) {
        const Bytes got = e.ask(read_of(kBase, 50));
        ASSERT_EQ(got.size(), whole.size());
        size_t at = 0;
        if (k % 2 != 0) {
            EXPECT_EQ(got, whole) << "reply " << k;
            continue;
        }
        EXPECT_EQ(bits_apart(got, whole, &at), 1) << "reply " << k;
        EXPECT_EQ(at, 1u + k % 50) << "the byte is the count's";
        EXPECT_EQ(got[at] ^ whole[at], 1 << ((k - 1) % 8))
            << "the bit is the pass's: each read of one address is a pass";
    }
    EXPECT_EQ(e.ask(read_of(kBase, 50))[0], 0x63)
        << "the service byte is never the one";

    Ecu u;
    u.m.faults.flip = 1;
    u.ask(upload_of(kBase, 100));
    const Bytes block = u.ask({0x36, 0x01});
    Bytes want = data_of(kBase, 100, u.m.vin);
    want[0] = 0x01;
    want.insert(want.begin(), 0x76);
    size_t at = 0;
    ASSERT_EQ(block.size(), want.size());
    EXPECT_EQ(bits_apart(block, want, &at), 1);
    EXPECT_EQ(at, 2u + 1u) << "past the counter";
}

/* Two passes over the same bytes never agree on a wrong one, whatever
 * the chunk, the count of replies in a pass and the period: a compare
 * of the two always tells */
TEST(SimMemoryTest, TwoPassesWithFlipNeverAgree)
{
    const struct {
        uint32_t chunk;
        uint32_t bytes;
        uint32_t flip;
    } reads[] = {{1, 2048, 8},    {1, 256, 1},    {2, 2048, 8},
                 {8, 512, 8},     {64, 4096, 4},  {256, 65536, 8},
                 {256, 65536, 1}, {100, 3000, 3}, {4094, 65536, 2}};
    for (const auto& r : reads) {
        Ecu e;
        e.m.faults.flip = r.flip;
        Bytes truth = data_of(kBase, r.bytes, e.m.vin);
        truth.erase(truth.begin()); /* the 63 */
        Bytes pass[2];
        for (Bytes& p : pass) {
            for (uint32_t at = 0; at < r.bytes; at += r.chunk) {
                const uint32_t n = std::min(r.chunk, r.bytes - at);
                const Bytes got = e.ask(read_of(kBase + at, n));
                ASSERT_EQ(got.size(), n + 1u);
                p.insert(p.end(), got.begin() + 1, got.end());
            }
        }
        const std::string what = "chunk " + std::to_string(r.chunk) + ", " +
                                 std::to_string(r.bytes) +
                                 " bytes, flip:" + std::to_string(r.flip);
        EXPECT_NE(pass[0], truth) << what;
        EXPECT_NE(pass[1], truth) << what;
        /* Every chunk either pass got wrong differs between the two */
        for (uint32_t at = 0; at < r.bytes; at += r.chunk) {
            const uint32_t n = std::min(r.chunk, r.bytes - at);
            const auto part = [at, n](const Bytes& b) {
                return Bytes(b.begin() + at, b.begin() + at + n);
            };
            if (part(pass[0]) != part(truth) || part(pass[1]) != part(truth)) {
                ASSERT_NE(part(pass[0]), part(pass[1])) << what << " at " << at;
            }
        }
    }

    /* An upload's two passes: 8 bytes a block, 64 blocks, every 8th wrong */
    Ecu u;
    u.m.faults.flip = 8;
    u.m.faults.block = 0x0A;
    Bytes pass[2];
    for (Bytes& p : pass) {
        ASSERT_EQ(u.ask(upload_of(kBase, 512))[0], 0x75);
        for (uint32_t k = 1; k <= 64; ++k) {
            const Bytes got = u.ask({0x36, static_cast<uint8_t>(k)});
            ASSERT_EQ(got.size(), 10u) << k;
            p.insert(p.end(), got.begin() + 2, got.end());
        }
        ASSERT_EQ(u.ask({0x37}), (Bytes{0x77}));
    }
    Bytes truth = data_of(kBase, 512, u.m.vin);
    truth.erase(truth.begin());
    EXPECT_NE(pass[0], truth);
    EXPECT_NE(pass[1], truth);
    for (uint32_t at = 0; at < 512; at += 8) {
        const auto part = [at](const Bytes& b) {
            return Bytes(b.begin() + at, b.begin() + at + 8);
        };
        if (part(pass[0]) != part(truth) || part(pass[1]) != part(truth)) {
            ASSERT_NE(part(pass[0]), part(pass[1])) << "block at " << at;
        }
    }
}

TEST(SimMemoryTest, AVolatileByteNeverReadsTheSameTwice)
{
    Ecu e;
    e.m.faults.moving = true;
    e.m.faults.moving_at = 0x20;
    const Bytes whole = data_of(kBase, 64, e.m.vin);
    Bytes last;
    for (int i = 0; i < 300; ++i) {
        const Bytes got = e.ask(read_of(kBase, 64));
        ASSERT_EQ(got.size(), whole.size());
        for (size_t k = 0; k < got.size(); ++k) {
            if (k != 1 + 0x20) {
                ASSERT_EQ(got[k], whole[k]) << "byte " << k;
            }
        }
        EXPECT_NE(got, last) << "read " << i;
        last = got;
    }
    EXPECT_EQ(e.ask(read_of(kBase + 0x21, 8)),
              data_of(kBase + 0x21, 8, e.m.vin))
        << "a read that leaves the byte out is the region's";
    EXPECT_EQ(e.ask(read_of(kBase, 0x20)), data_of(kBase, 0x20, e.m.vin));
    EXPECT_NE(e.ask(read_of(kBase + 0x20, 1)), e.ask(read_of(kBase + 0x20, 1)));

    Ecu u;
    u.m.faults.moving = true;
    u.m.faults.moving_at = 0x500;
    u.ask(upload_of(kBase, 2048));
    const Bytes first = u.ask({0x36, 0x01});
    const Bytes second = u.ask({0x36, 0x02});
    ASSERT_EQ(second.size(), 0x402u);
    const Bytes region = data_of(kBase, 1024, u.m.vin);
    EXPECT_EQ(Bytes(first.begin() + 2, first.end()),
              Bytes(region.begin() + 1, region.end()));
    EXPECT_EQ(second[2 + 0x100], 0x01) << "its first read, in the second block";
}

TEST(SimMemoryTest, RelockTakesTheUnlockOnceTheNthDataReplyIsMade)
{
    Ecu e;
    e.m.faults.relock = 3;
    const Bytes read = read_of(kBase, 16);
    const Bytes whole = data_of(kBase, 16, e.m.vin);
    EXPECT_EQ(e.ask(read), whole);
    EXPECT_EQ(e.ask(read), whole);
    EXPECT_EQ(e.ask(read), whole) << "the nth is still answered";
    EXPECT_EQ(e.s.unlocked, 0);
    EXPECT_EQ(e.ask(read), nrc(0x23, 0x33));
    EXPECT_EQ(e.ask(read), nrc(0x23, 0x33));
    e.s.unlocked = 0x01;
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(e.ask(read), whole) << "it strikes once";
    }

    Ecu u;
    u.m.faults.relock = 1;
    u.ask(upload_of(kBase, 3000));
    EXPECT_EQ(u.ask({0x36, 0x01}).size(), 0x402u);
    EXPECT_EQ(u.ask({0x36, 0x02}), nrc(0x36, 0x33));
}

TEST(SimMemoryTest, SeqGivesEveryNthBlockTheWrongCounter)
{
    Ecu e;
    e.m.faults.seq = 2;
    e.ask(upload_of(kBase, 4096));
    Bytes image;
    for (uint8_t n = 1; n <= 4; ++n) {
        const Bytes r = e.ask({0x36, n});
        ASSERT_EQ(r.size(), 0x402u);
        EXPECT_EQ(r[1], n % 2 == 0 ? n + 1 : n) << "block " << int(n);
        image.insert(image.end(), r.begin() + 2, r.end());
    }
    const Bytes want = data_of(kBase, 4096, e.m.vin);
    EXPECT_EQ(image, Bytes(want.begin() + 1, want.end()))
        << "the data is right and the ECU's own counter moved by one";
    EXPECT_EQ(e.ask({0x37}), (Bytes{0x77}));
    EXPECT_EQ(e.ask(read_of(kBase, 8)), data_of(kBase, 8, e.m.vin))
        << "a read has no counter to break";
}

TEST(SimMemoryTest, RefuseAnswersEveryNthDataReplyConditionsNotCorrect)
{
    Ecu e;
    e.m.faults.refuse = 2;
    const Bytes read = read_of(kBase, 16);
    EXPECT_EQ(e.ask(read), data_of(kBase, 16, e.m.vin));
    EXPECT_EQ(e.ask(read), nrc(0x23, 0x22));
    EXPECT_EQ(e.ask(read), data_of(kBase, 16, e.m.vin));
    EXPECT_EQ(e.ask(read), nrc(0x23, 0x22));

    /* A refused block leaves the upload where it was */
    e.ask(upload_of(kBase, 3000));
    EXPECT_EQ(e.ask({0x36, 0x01}).size(), 0x402u);
    EXPECT_EQ(e.ask({0x36, 0x02}), nrc(0x36, 0x22));
    EXPECT_EQ(e.m.left, 3000u - 1024u);
    const Bytes again = e.ask({0x36, 0x02});
    ASSERT_EQ(again.size(), 0x402u);
    const Bytes want = data_of(kBase + 1024, 1024, e.m.vin);
    EXPECT_EQ(Bytes(again.begin() + 2, again.end()),
              Bytes(want.begin() + 1, want.end()));

    /* Refused, a reply is neither cut nor changed, and still relocks */
    Ecu both;
    both.m.faults.refuse = 1;
    both.m.faults.cut = 1;
    both.m.faults.flip = 1;
    both.m.faults.relock = 1;
    EXPECT_EQ(both.ask(read), nrc(0x23, 0x22));
    EXPECT_EQ(both.s.unlocked, 0);
}

TEST(SimMemoryTest, IdentChangesTheSoftwareDidFromTheNthDataReply)
{
    Node n;
    n.s.memory.faults.ident = 2;
    n.ask({0x10, 0x03});
    n.unlock();
    const Bytes software = ident_of(0xD221, "HAVNESIM-SW-0001");
    EXPECT_EQ(n.ask(kSoftware), software);
    n.ask(read_of(kBase, 16));
    EXPECT_EQ(n.ask(kSoftware), software);
    n.ask(read_of(kBase, 16));
    EXPECT_EQ(n.ask(kSoftware), ident_of(0xD221, "HAVNESIM-SW-0002"));
    EXPECT_EQ(n.ask(kHardware), ident_of(0xD220, "HAVNESIM-HW-0001"))
        << "the hardware's never changes";
    n.ask({0x10, 0x03});
    EXPECT_EQ(n.ask(kSoftware), ident_of(0xD221, "HAVNESIM-SW-0002"))
        << "a new session doesn't put it back";
}

/* ---------- through the unbound sim ---------- */

TEST(SimMemoryTest, AnUnboundSimTakesTheMemKey)
{
    Unbound u("mock://ask?values=canned&mem=refuse:2,block:82&mem=ident:1");
    ASSERT_NE(u.sim, nullptr);
    u.unlock();
    EXPECT_EQ(u.ask(0x7E0, upload_of(kBase, 256)),
              (Bytes{0x75, 0x20, 0x00, 0x82}));
    EXPECT_EQ(u.ask(0x7E0, {0x36, 0x01}).size(), 0x82u);
    EXPECT_EQ(u.ask(0x7E0, {0x36, 0x02}), nrc(0x36, 0x22));
    EXPECT_EQ(u.ask(0x7E0, {0x36, 0x02}).size(), 0x82u);
    EXPECT_EQ(u.ask(0x7E0, kSoftware), ident_of(0xD221, "HAVNESIM-SW-0002"));

    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound("mock://ask?mem=short:0", 0, &sim),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(sim, nullptr);
    EXPECT_EQ(std::string(ecuio_last_error_detail()),
              std::string("mem: 'short:0'") + kGrammar);
}

TEST(SimMemoryTest, AMemKeyIsRefusedWhereAProfileAnswersForTheEngineEcu)
{
    /* A set whose one module is the engine ECU's pair: its node answers
     * the set's DIDs and has no security access, so no memory */
    static ecuio_signal_def_t def{};
    static ecuio_channel_decl_t channel{};
    static ecuio_signal_set_t set{};
    def.id = 100;
    def.message_id = 0x7E8;
    def.mux_length = 3;
    def.mux_key = 0x621000u;
    def.kind = ECUIO_VALUE_UINT;
    def.byte_order = ECUIO_BYTE_ORDER_BE;
    def.start_byte = 3;
    def.bit_length = 8;
    def.scale = 1.0;
    std::snprintf(def.name, sizeof def.name, "Engine");
    channel.rx_id = 0x7E8;
    channel.tx_id = 0x7E0;
    set.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    set.count = 1;
    set.defs = &def;
    set.channel_count = 1;
    set.channels = &channel;
    ecuio_validation_t v{};
    ASSERT_EQ(ecuio_profile_register("sim-memory-engine", &set, &v), ECUIO_OK)
        << v.code;
    ecuio_sim_t* sim = nullptr;
    EXPECT_EQ(ecuio_sim_create_unbound(
                  "mock://ask?profile=sim-memory-engine&mem=flip:2", 0, &sim),
              ECUIO_ERR_BAD_URI);
    EXPECT_EQ(sim, nullptr);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "mem: the engine ECU has no memory to break while a "
                 "profile's set answers for it");
    {
        Unbound u("mock://ask?profile=sim-memory-engine");
        ASSERT_NE(u.sim, nullptr);
        EXPECT_EQ(u.ask(0x7E0, read_of(kBase, 16)), nrc(0x23, 0x11))
            << "and that engine ECU has none to read";
    }
    EXPECT_EQ(ecuio_profile_unregister("sim-memory-engine"), ECUIO_OK);
}

/* ---------- the fakes' ECU (tests/fake_ecus.h) ---------- */

TEST_F(FakeMemoryTest, TheMemoryFaultsKnobBreaksEachEcusReadsOnItsOwnCount)
{
    const auto unlock = [](uint32_t rx) {
        ask(rx, {0x10, 0x03});
        ask(rx, {0x27, 0x01});
        EXPECT_EQ(ask(rx, key_for(0x01)), (Bytes{0x67, 0x02}));
    };
    EXPECT_FALSE(fake_ecu::set_memory_faults("bogus:1"));
    ASSERT_TRUE(fake_ecu::set_memory_faults("refuse:2"));
    EXPECT_FALSE(fake_ecu::set_memory_faults("refuse:0"))
        << "refused words leave the knob as it was";
    unlock(0x7E8);
    unlock(0x7E9);
    const Bytes read = read_of(kBase, 16);
    const Bytes whole = data_of(kBase, 16, fake_ecu::kVin);
    EXPECT_EQ(ask(0x7E8, read), whole);
    EXPECT_EQ(ask(0x7E9, read), whole) << "the first of its own";
    EXPECT_EQ(ask(0x7E8, read), nrc(0x23, 0x22));
    EXPECT_EQ(ask(0x7E9, read), nrc(0x23, 0x22));

    ASSERT_TRUE(fake_ecu::set_memory_faults(""));
    EXPECT_EQ(ask(0x7E8, read), whole);
    EXPECT_EQ(ask(0x7E8, read), whole) << "the knob put back";
    ASSERT_TRUE(fake_ecu::set_memory_faults("block:82,ident:1"));
    EXPECT_EQ(ask(0x7E8, upload_of(kBase, 256)),
              (Bytes{0x75, 0x20, 0x00, 0x82}));
    EXPECT_EQ(ask(0x7E8, kSoftware), ident_of(0xD221, "HAVNESIM-SW-0002"));
}

} // namespace
