// File/layer: tests/test_dump_record.cpp | C++17, Qt-free, every platform
// Testing: UNIT: DumpFileTest (test_core)
//
// A dump file's header and record as the reader judges them
// (core/src/dump_file.h): each refusal by its words, every truncation, the
// fullest record, and a copy's name.

#include "dump_file_support.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dump_file_test;
using ecuio_dump_file::judged;

/* text with its one from replaced by to */
std::string edited(const std::string& text, const std::string& from,
                   const std::string& to)
{
    const size_t at = text.find(from);
    if (at == std::string::npos ||
        text.find(from, at + 1) != std::string::npos) {
        ADD_FAILURE() << "the record holds " << from << " "
                      << (at == std::string::npos ? "nowhere" : "twice");
        return text;
    }
    std::string out = text;
    out.replace(at, from.size(), to);
    return out;
}

/* What the reader says of a file in memory; "" when it takes it */
std::string judge(const std::string& file)
{
    ecuio_dump_file::header h;
    ecuio_dump_file::record r;
    std::string why;
    const judged j =
        ecuio_dump_file::parse(reinterpret_cast<const uint8_t*>(file.data()),
                               file.size(), &h, &r, &why);
    EXPECT_NE(j, judged::out_of_memory);
    EXPECT_EQ(j == judged::ok, why.empty());
    return why;
}

std::string judge_text(const std::string& text)
{
    return judge(file_with(text, image_of(kImageBytes)));
}

void put32(std::string& file, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        file[at + i] = static_cast<char>(v >> (8 * i));
    }
}

TEST(DumpFileTest, TheReaderTakesWhatTheWriterWrites)
{
    const ecuio_dump_file::record r = record_of();
    const std::string file = file_of(r);
    EXPECT_EQ(judge(file), "");
    ecuio_dump_file::header h;
    ecuio_dump_file::record back;
    std::string why;
    ASSERT_EQ(
        ecuio_dump_file::parse(reinterpret_cast<const uint8_t*>(file.data()),
                               file.size(), &h, &back, &why),
        judged::ok);
    ecuio_dump_file::record want = r; /* build sets the image's two */
    want.image_bytes = back.image_bytes;
    std::memcpy(want.image_sha256, back.image_sha256, 32);
    EXPECT_TRUE(ecuio_dump_file::same(want, back));
    want.seconds += 1;
    EXPECT_FALSE(ecuio_dump_file::same(want, back));
    EXPECT_EQ(back.image_bytes, kImageBytes);
    EXPECT_EQ(back.ranges[1].name, "boot");
    EXPECT_EQ(back.idents[1].value, (std::vector<uint8_t>{0x00, 0xFF, 0x7A}));
    EXPECT_EQ(ecuio_dump_file::record_text(back), text_of(r))
        << "written again, the same text";
    EXPECT_EQ(file.size(), 128 + kImageBytes + h.record_bytes);
    EXPECT_EQ(static_cast<uint8_t>(file[0]), 0x89);
    EXPECT_EQ(file.substr(1, 3), "HVD");
    EXPECT_EQ(file.substr(4, 4), "\r\n\x1A\n");
}

/* A file in memory is judged whole: its image against the header too */
TEST(DumpFileTest, AFileInMemoryWithADamagedImageIsRefused)
{
    std::string file = file_of(record_of());
    for (const size_t at : {size_t{128}, size_t{128 + kImageBytes - 1}}) {
        file[at] = static_cast<char>(file[at] ^ 0x10);
        EXPECT_EQ(judge(file), "its image isn't the one its header seals")
            << "byte " << at;
        file[at] = static_cast<char>(file[at] ^ 0x10);
    }
    EXPECT_EQ(judge(file), "");
}

TEST(DumpFileTest, TheFullestRecordFitsItsBound)
{
    ecuio_dump_file::record r = record_of();
    r.ranges.clear();
    for (uint32_t i = 0; i < ECUIO_DUMP_MAX_RANGES; ++i) {
        ecuio_dump_file::range g;
        g.name = std::string(31, 'a');
        g.step = ECUIO_DUMP_STEP_UPLOAD;
        g.session = ECUIO_DUMP_SESSION_PROGRAMMING;
        g.address = 0xFF000000u + i * 0x1000u;
        g.bytes = 12;
        g.offset = i * 12;
        r.ranges.push_back(g);
    }
    r.idents.clear();
    for (uint32_t i = 0; i < ECUIO_DUMP_MAX_IDENTS; ++i) {
        ecuio_dump_file::ident d;
        d.did = 0xF100 + i;
        d.role = 1 + i % 3;
        d.value.assign(ECUIO_DUMP_IDENT_MAX, 0xAB);
        r.idents.push_back(d);
    }
    r.calids.assign(ECUIO_DUMP_MAX_CALIBRATIONS, std::string(16, 'C'));
    r.cvns.assign(ECUIO_DUMP_MAX_CALIBRATIONS, {0xFF, 0xFF, 0xFF, 0xFF});
    r.ecu_name = std::string(20, 'N');
    r.plugin_id = std::string(31, 'p');
    r.plugin_version = "65535.65535.655";
    r.operation = std::string(31, 'o');
    r.scheme = std::string(15, 's');
    r.adapter = std::string(63, 'A');
    r.firmware = std::string(63, 'F');
    r.rx_id = 0x18DAF110;
    r.tx_id = 0x18DA10F1;
    r.utc = ecuio_dump_file::kMaxUtc;
    r.seconds = UINT32_MAX;
    r.writes_before = UINT32_MAX;
    r.unlocks = 64;
    r.volts_mv = r.volts_min_mv = r.rpm = r.kmh = 100000;
    const Bytes image = image_of(ECUIO_DUMP_MAX_RANGES * 12);
    const std::string file =
        ecuio_dump_file::build(r, image.data(), image.size());
    EXPECT_EQ(judge(file), "");
    const size_t text = file.size() - 128 - image.size();
    EXPECT_LE(text, size_t{ECUIO_DUMP_RECORD_MAX});
    EXPECT_GT(text, size_t{8000}) << "and it is the long one";
    uint8_t sum[32] = {};
    EXPECT_EQ(ecuio_dump_file::name_of(r.utc, r.tx_id, sum).size(), 92u);
}

TEST(DumpFileTest, EveryTruncationOfASmallFileIsRefused)
{
    const std::string file = file_of(record_of());
    for (size_t n = 0; n < file.size(); ++n) {
        const std::string why = judge(file.substr(0, n));
        ASSERT_FALSE(why.empty()) << n << " bytes";
        if (n < 128) {
            ASSERT_EQ(why, "it is shorter than a dump's header") << n;
        } else {
            ASSERT_EQ(why.rfind("its sizes don't add up: ", 0), 0u) << n;
        }
    }
    EXPECT_EQ(judge(file + "x").rfind("its sizes don't add up: ", 0), 0u)
        << "a byte too many";

    Folder f;
    for (const size_t n : {size_t{0}, size_t{1}, size_t{127}, size_t{128},
                           size_t{129}, file.size() / 2, file.size() - 1}) {
        EXPECT_FALSE(open_refusal(f, file.substr(0, n)).empty()) << n;
    }
    EXPECT_EQ(open_refusal(f, file.substr(0, file.size() - 1)),
              "its sizes don't add up: the header says " +
                  std::to_string(file.size()) + " bytes and the file has " +
                  std::to_string(file.size() - 1));
}

TEST(DumpFileTest, EachHeaderRefusalNamesItsCause)
{
    const std::string file = file_of(record_of());
    const auto with = [&file](size_t at, uint32_t v) {
        std::string f = file;
        put32(f, at, v);
        return f;
    };
    std::string magic = file;
    magic[1] = 'h';
    EXPECT_EQ(judge(magic),
              "it isn't a dump file: its first bytes aren't a dump's");
    /* Copied as text: the magic's CR LF became LF */
    EXPECT_EQ(judge(file.substr(0, 4) + file.substr(5)),
              "it isn't a dump file: its first bytes aren't a dump's");
    EXPECT_EQ(judge(with(8, 2)),
              "it was made by a later Havne (format 2): this one reads "
              "format 1");
    std::string later = with(8, 7);
    put32(later, 28, 1);
    put32(later, 12, 4096);
    later.resize(200);
    EXPECT_EQ(judge(later),
              "it was made by a later Havne (format 7): this one reads "
              "format 1")
        << "judged before anything else of the file";
    EXPECT_EQ(judge(with(8, 0)), "its format is 0");
    EXPECT_EQ(judge(with(12, 127)), "its header isn't 128 bytes");
    EXPECT_EQ(judge(with(28, 1)), "its header's flags aren't zero");
    for (const size_t at : {size_t{96}, size_t{110}, size_t{127}}) {
        std::string reserved = file;
        reserved[at] = 1;
        EXPECT_EQ(judge(reserved), "its header's reserved bytes aren't zero")
            << at;
    }
    EXPECT_EQ(judge(with(16, 0)),
              "its image is 0 bytes: a dump holds 1 to 67108864");
    EXPECT_EQ(judge(with(16, ECUIO_DUMP_IMAGE_MAX + 1)),
              "its image is 67108865 bytes: a dump holds 1 to 67108864");
    EXPECT_EQ(judge(with(20, 1)),
              "its image is 4294968064 bytes: a dump holds 1 to 67108864")
        << "the length's high half is read";
    EXPECT_EQ(judge(with(24, 0)),
              "its record is 0 bytes: a dump's is 1 to 65536");
    EXPECT_EQ(judge(with(24, ECUIO_DUMP_RECORD_MAX + 1)),
              "its record is 65537 bytes: a dump's is 1 to 65536");
    EXPECT_EQ(judge(with(16, kImageBytes + 1)),
              "its sizes don't add up: the header says " +
                  std::to_string(file.size() + 1) + " bytes and the file has " +
                  std::to_string(file.size()));
    /* A length the file's size doesn't bear is refused before a byte of
     * it is read or allocated for */
    EXPECT_EQ(judge(with(16, ECUIO_DUMP_IMAGE_MAX)).rfind("its sizes", 0), 0u);
    EXPECT_EQ(judge(with(24, ECUIO_DUMP_RECORD_MAX)).rfind("its sizes", 0), 0u);
}

TEST(DumpFileTest, EachRecordRefusalNamesItsCause)
{
    const std::string text = text_of(record_of());
    struct row {
        const char* from;
        const char* to;
        const char* words;
    };
    const row rows[] = {
        {"\"havne_dump\": 1", "\"havne_dump\": 2",
         "\"havne_dump\" isn't the header's format"},
        {"\"kind\": \"read\"", "\"kind\": \"written\"",
         "this Havne reads a dump of kind \"read\" only"},
        {"\"utc\": 1795312345", "\"utc\": 0",
         "\"utc\" takes a whole number from 1 to 253402300799"},
        {"\"utc\": 1795312345", "\"utc\": 253402300800",
         "\"utc\" takes a whole number from 1 to 253402300799"},
        {"\"core\": \"0.9.1\"", "\"core\": \"0.9\"", "\"core\" isn't X.Y.Z"},
        {"\"kind\": \"read\",", "\"kind\": \"read\", \"note\": 1,",
         "unknown key 'note' in a dump's record"},
        {"\"kind\": \"read\",", "\"kind\": \"read\", \"x-tool\": 1,",
         "unknown key 'x-tool' in a dump's record"},
        {"\"kind\": \"read\",", "\"kind\": \"read\", \"notes\": \"mine\",",
         "unknown key 'notes' in a dump's record"},
        {"\"core\": \"0.9.1\",", "", "a dump's record needs \"core\""},
        {"\"bytes\": 768", "\"bytes\": 767",
         "the record's image is 767 bytes and the header's 768"},
        /* ranges */
        {"\"offset\": 512", "\"offset\": 513",
         "the ranges don't tile the image: this one starts at 513, not 512"},
        {"\"bytes\": 256", "\"bytes\": 255",
         "the ranges don't tile the image: they add to 767 bytes and the "
         "image has 768"},
        {"\"bytes\": 256", "\"bytes\": 257",
         "the ranges don't tile the image: they add to 769 bytes and the "
         "image has 768"},
        {"\"bytes\": 512", "\"bytes\": 0",
         "a range's bytes takes a whole number from 1 to 67108864"},
        {"\"address\": \"00020000\"", "\"address\": \"000101FF\"",
         "two ranges overlap in address"},
        {"\"address\": \"00020000\"", "\"address\": \"0000FF01\"",
         "two ranges overlap in address"},
        {"\"address\": \"00020000\"", "\"address\": \"0002000a\"",
         "a range's address takes pairs of 0-9 and A-F"},
        {"\"address\": \"00020000\"", "\"address\": \"0020000\"",
         "a range's address is 7 bytes; it takes 8 to 8"},
        {"\"address\": \"00010000\"", "\"address\": \"FFFFFF00\"",
         "a range runs past the module's 32-bit addresses"},
        {"\"name\": \"boot\"", "\"name\": \"Boot\"",
         "a range's name takes a-z, 0-9 and '-'"},
        {"\"step\": \"upload\"", "\"step\": \"download\"",
         "a range's step is one of \"read\", \"upload\""},
        {"\"session\": \"programming\"", "\"session\": \"default\"",
         "a range's session is one of \"programming\", \"extended\""},
        /* the module */
        {"\"rx\": \"7E8\"", "\"rx\": \"7e8\"",
         "the module's rx takes 3 hex digits (11-bit) or 8 (29-bit)"},
        {"\"rx\": \"7E8\"", "\"rx\": \"800\"",
         "the module's rx takes 3 hex digits (11-bit) or 8 (29-bit)"},
        {"\"rx\": \"7E8\"", "\"rx\": \"07E8\"",
         "the module's rx takes 3 hex digits (11-bit) or 8 (29-bit)"},
        {"\"tx\": \"7E0\"", "\"tx\": \"20000000\"",
         "the module's tx takes 3 hex digits (11-bit) or 8 (29-bit)"},
        {"\"rx\": \"7E8\"", "\"rx\": \"000007E8\"",
         "the module's rx and tx differ in width"},
        {"\"role\": \"base\"", "\"role\": \"boot\"",
         "an identification's role is one of \"hardware\", \"software\", "
         "\"base\""},
        {"\"hex\": \"00FF7A\"", "\"hex\": \"00FF7\"",
         "an identification's hex takes pairs of 0-9 and A-F"},
        {"\"hex\": \"00FF7A\"", "\"hex\": \"00ff7a\"",
         "an identification's hex takes pairs of 0-9 and A-F"},
        {"\"hex\": \"00FF7A\"", "\"hex\": \"\"",
         "an identification's hex is 0 bytes; it takes 2 to 128"},
        {"\"did\": \"D221\"", "\"did\": \"D220\"",
         "an identification's did is given twice"},
        {"\"did\": \"D221\"", "\"did\": \"D22\"",
         "an identification's did is 3 bytes; it takes 4 to 4"},
        {"\"vin\": \"ZZZFAKE11M0000001\"", "\"vin\": \"ZZZFAKE11M000000I\"",
         "\"obd\"'s vin takes 17 digits and capital letters, never I, O or Q"},
        {"\"vin\": \"ZZZFAKE11M0000001\"", "\"vin\": \"ZZZFAKE11M000001\"",
         "\"obd\"'s vin is 16 bytes; it takes 17 to 17"},
        {"\"DEADBEEF\"", "\"DEADBEE\"", "a cvn is 7 bytes; it takes 8 to 8"},
        {"\"FAKECAL-02\"", "\"\"", "a calid is 0 bytes; it takes 1 to 16"},
        {"\"name\": \"ECM-HavneSim\"", "\"name\": \"ECM-HavneSim-123456789\"",
         "\"obd\"'s name is 22 bytes; it takes 1 to 20"},
        /* the plugin and the transport */
        {"\"id\": \"example-car\"", "\"id\": \"Example\"",
         "the plugin's id takes a-z, 0-9 and '-'"},
        {"\"version\": \"1.2.0\"", "\"version\": \"1.2\"",
         "the plugin's version isn't X.Y.Z"},
        {"0a0b0c0d", "0A0b0c0d",
         "the plugin's module_sha256 takes pairs of 0-9 and a-f"},
        {"\"fingerprint\": \"f0", "\"fingerprint\": \"f",
         "the plugin's fingerprint is 63 bytes; it takes 64 to 64"},
        {"\"operation\": \"dump-calibration\"", "\"operation\": \"-x\"",
         "the plugin's operation takes a-z, 0-9 and '-'"},
        {"\"scheme\": \"j2534\"", "\"scheme\": \"J2534\"",
         "the transport's scheme takes a-z and 0-9"},
        {"\"adapter\": \"Made-up Adapter\"", "\"adapter\": \"Made\\u0007up\"",
         "the transport's adapter"},
        {"\"firmware\": \"1.0\"", "\"firmware\": 1",
         "the transport's firmware is"},
        /* how it was read */
        {"\"passes\": 2", "\"passes\": 1",
         "\"read\"'s passes is 2: a copy is read twice"},
        {"\"passes\": 2", "\"passes\": 3",
         "\"read\"'s passes takes a whole number from 0 to 2"},
        {"\"seconds\": 41", "\"seconds\": -1",
         "\"read\"'s seconds takes a whole number from 0 to 4294967295"},
        {"\"seconds\": 41", "\"seconds\": 41.5",
         "\"read\"'s seconds takes a whole number from 0 to 4294967295"},
        {"\"volts_mv\": 12450", "\"volts_mv\": 100001",
         "volts_mv takes a whole number from 0 to 100000"},
        {"\"rpm\": 0", "\"rpm\": 100001",
         "rpm takes a whole number from 0 to 100000"},
        {"\"parked_tick\": true", "\"parked_tick\": 1",
         "\"read\"'s parked_tick is"},
        {"\"unlocks\": 1", "\"unlocks\": 65",
         "\"read\"'s unlocks takes a whole number from 0 to 64"},
        {"\"writes_before\": 0", "\"writes_before\": 4294967296",
         "\"read\"'s writes_before takes a whole number from 0 to 4294967295"},
        {"\"unlocks\": 1,", "\"unlocks\": 1, \"note\": 1,",
         "unknown key 'note' in \"read\""},
        {"\"parked_tick\": true,", "", "\"read\" needs \"parked_tick\""},
    };
    ASSERT_EQ(judge_text(text), "") << "the text these rows change is taken";
    for (const row& r : rows) {
        const std::string why = judge_text(edited(text, r.from, r.to));
        EXPECT_EQ(why.rfind("its record, line ", 0), 0u)
            << r.from << " -> " << r.to << ": " << why;
        EXPECT_NE(why.find(std::string(": ") + r.words), std::string::npos)
            << r.from << " -> " << r.to << ": " << why;
    }

    /* The format is judged before the keys: a later record is refused
     * as that, not for a key this Havne doesn't know */
    EXPECT_NE(
        judge_text(
            edited(edited(text, "\"havne_dump\": 1", "\"havne_dump\": 2"),
                   "\"kind\": \"read\",", "\"kind\": \"read\", \"later\": 1,"))
            .find("\"havne_dump\" isn't the header's format"),
        std::string::npos);
    /* No JSON, and a byte order mark */
    EXPECT_EQ(judge_text("{").rfind("its record, line 1, column ", 0), 0u)
        << judge_text("{");
    EXPECT_EQ(judge_text("\xEF\xBB\xBF" + text).rfind("its record, line ", 0),
              0u);
    EXPECT_EQ(judge_text("[]").find("a dump's record is"),
              std::string("its record, line 1, column 1: ").size());
    /* The image's checksum, one digit changed */
    const size_t sha = text.find("\"sha256\": \"") + 11;
    std::string other = text;
    other[sha] = other[sha] == '0' ? '1' : '0';
    EXPECT_NE(judge_text(other).find(
                  "the record's image checksum isn't the header's"),
              std::string::npos);
    /* "obd" holding nothing */
    const size_t obd = text.find("\"obd\": {");
    ASSERT_NE(obd, std::string::npos);
    std::string empty = text;
    empty.replace(obd, text.find('}', obd) + 1 - obd, "\"obd\": {}");
    EXPECT_NE(judge_text(empty).find(
                  "\"obd\" holds nothing: a dump leaves it out instead"),
              std::string::npos);
}

TEST(DumpFileTest, EachCountsBoundIsHeld)
{
    const auto why_of = [](const ecuio_dump_file::record& r) {
        return judge(file_of(r));
    };
    ecuio_dump_file::record r = record_of();
    r.ranges.clear();
    EXPECT_NE(why_of(r).find("\"ranges\" lists 0; a dump holds 1 to 64"),
              std::string::npos);
    for (uint32_t i = 0; i < 65; ++i) {
        ecuio_dump_file::range g;
        g.name = "r";
        g.address = 0x1000u * i;
        g.bytes = i < 64 ? 1 : kImageBytes - 64;
        g.offset = i;
        r.ranges.push_back(g);
    }
    EXPECT_NE(why_of(r).find("\"ranges\" lists 65; a dump holds 1 to 64"),
              std::string::npos);
    r.ranges.pop_back();
    r.ranges.back().bytes = kImageBytes - 63;
    EXPECT_EQ(why_of(r), "") << "64 are taken";

    r = record_of();
    r.idents.resize(8, r.idents[0]);
    for (uint32_t i = 0; i < 8; ++i) {
        r.idents[i].did = 0xF100 + i;
    }
    EXPECT_EQ(why_of(r), "");
    r.idents.push_back(r.idents[0]);
    r.idents.back().did = 0xF1FF;
    EXPECT_NE(why_of(r).find("\"ident\" lists 9, more than 8"),
              std::string::npos);

    r = record_of();
    r.idents[0].value.assign(64, 0x11);
    EXPECT_EQ(why_of(r), "");
    r.idents[0].value.assign(65, 0x11);
    EXPECT_NE(why_of(r).find(
                  "an identification's hex is 130 bytes; it takes 2 to 128"),
              std::string::npos);

    r = record_of();
    r.calids.assign(8, "C");
    r.cvns.assign(8, {1, 2, 3, 4});
    EXPECT_EQ(why_of(r), "");
    r.calids.assign(9, "C");
    EXPECT_NE(why_of(r).find("\"calids\" lists 9; it takes 1 to 8"),
              std::string::npos);
    r.calids.assign(1, "C");
    r.cvns.assign(9, {1, 2, 3, 4});
    EXPECT_NE(why_of(r).find("\"cvns\" lists 9; it takes 1 to 8"),
              std::string::npos);

    r = record_of();
    r.adapter = std::string(63, 'a');
    EXPECT_EQ(why_of(r), "");
    r.adapter = std::string(64, 'a');
    EXPECT_NE(why_of(r).find("the transport's adapter is 64 bytes"),
              std::string::npos);
}

TEST(DumpFileTest, ACopysNameIsItsDateItsIdAndItsChecksum)
{
    uint8_t sum[32];
    for (int i = 0; i < 32; ++i) {
        sum[i] = static_cast<uint8_t>(0xA0 + i);
    }
    const std::string hex =
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";
    using ecuio_dump_file::name_of;
    EXPECT_EQ(name_of(kUtc, 0x7E0, sum), "20261122-7E0-" + hex + ".havnedump");
    EXPECT_EQ(name_of(kUtc, 0x18DA10F1, sum),
              "20261122-18DA10F1-" + hex + ".havnedump");
    EXPECT_EQ(name_of(kUtc, 0x00A, sum).substr(9, 3), "00A");
    EXPECT_EQ(name_of(kUtc, 0x800, sum).substr(9, 8), "00000800")
        << "past 11 bits, 8 digits";
    /* The date is UTC's, from the record's second */
    EXPECT_EQ(name_of(1, 0x7E0, sum).substr(0, 8), "19700101");
    EXPECT_EQ(name_of(86399, 0x7E0, sum).substr(0, 8), "19700101");
    EXPECT_EQ(name_of(86400, 0x7E0, sum).substr(0, 8), "19700102");
    EXPECT_EQ(name_of(951782400, 0x7E0, sum).substr(0, 8), "20000229");
    EXPECT_EQ(name_of(1709164800, 0x7E0, sum).substr(0, 8), "20240229");
    EXPECT_EQ(name_of(1709251199, 0x7E0, sum).substr(0, 8), "20240229");
    EXPECT_EQ(name_of(1709251200, 0x7E0, sum).substr(0, 8), "20240301");
    EXPECT_EQ(name_of(4107542399, 0x7E0, sum).substr(0, 8), "21000228");
    EXPECT_EQ(name_of(4107542400, 0x7E0, sum).substr(0, 8), "21000301")
        << "2100 is no leap year";
    EXPECT_EQ(name_of(ecuio_dump_file::kMaxUtc, 0x7E0, sum).substr(0, 8),
              "99991231");

    using ecuio_dump_file::name_hash;
    uint8_t out[32] = {};
    const std::string name = name_of(kUtc, 0x7E0, sum);
    EXPECT_TRUE(name_hash(name, out));
    EXPECT_EQ(std::memcmp(out, sum, 32), 0);
    EXPECT_TRUE(name_hash(name_of(kUtc, 0x18DA10F1, sum), out));
    for (const std::string& bad :
         {std::string("x.havnedump"), hex + ".havnedump",
          name.substr(0, name.size() - 1), name + "x",
          edited(name, ".havnedump", ".HAVNEDUMP"),
          edited(name, "20261122-", "2026112x-"),
          edited(name, "20261122-", "20261122_"),
          edited(name, "-7E0-", "-7e0-"), edited(name, "-7E0-", "-07E0-"),
          edited(name, "-7E0-", "-7E0_"), edited(name, "a0a1", "A0a1"),
          edited(name, "a0a1", "g0a1"), std::string()}) {
        EXPECT_FALSE(name_hash(bad, out)) << bad;
    }

    using ecuio_dump_file::partial_name;
    EXPECT_TRUE(partial_name(name + ".partial-123-4"));
    EXPECT_FALSE(partial_name(name));
    EXPECT_FALSE(partial_name("frames.csv.partial-123-4"));
}

} // namespace
