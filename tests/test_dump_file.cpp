// File/layer: tests/test_dump_file.cpp | C++17, Qt-free, every platform
// Testing: UNIT: DumpFileTest (test_core)
//
// A dump file on the disk (ecuio/dump.h): a built file read back through
// every getter, the image read in pieces, verify's three checks and what
// each catches, a folder's list and its three states, the getters'
// contract and the out-of-memory loop.

#include "dump_file_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* test_core.cpp's, over tests/new_counter.h */
unsigned long news_on_this_thread();
void fail_new_on_this_thread(unsigned long n);

namespace {

using namespace dump_file_test;

const char kDamaged[] =
    "its bytes aren't the ones its header seals: the file is damaged";
const char kRenamed[] = "it is sound, but its name isn't its checksum's";

/* verify's status for d, the name it gives in *name */
ecuio_status_t verify(const Dump& d, std::string* name = nullptr)
{
    char out[ECUIO_DUMP_NAME_MAX];
    std::memset(out, 'x', sizeof out);
    const ecuio_status_t s =
        ecuio_dump_verify(d.get(), nullptr, nullptr, out, sizeof out);
    if (name != nullptr) {
        *name = std::string(out, strnlen(out, sizeof out));
    }
    return s;
}

std::string flipped(std::string bytes, size_t at, int bit = 0)
{
    bytes[at] = static_cast<char>(bytes[at] ^ (1 << bit));
    return bytes;
}

TEST(DumpFileTest, ABuiltFileReadsBackThroughEveryGetter)
{
    Folder f;
    const ecuio_dump_file::record r = record_of();
    const std::string bytes = file_of(r);
    const Dump d = open_dump(f.put_copy(bytes));
    ASSERT_NE(d, nullptr);

    ecuio_dump_info_t info;
    std::memset(&info, 0xA5, sizeof info);
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_dump_get_info(d.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.struct_size, sizeof info);
    EXPECT_EQ(info.flags, ECUIO_DUMP_HAS_VOLTS | ECUIO_DUMP_HAS_VOLTS_MIN |
                              ECUIO_DUMP_HAS_RPM | ECUIO_DUMP_HAS_KMH |
                              ECUIO_DUMP_PARKED_TICK | ECUIO_DUMP_HAS_OBD);
    EXPECT_EQ(info.format, 1u);
    EXPECT_EQ(info.kind, ECUIO_DUMP_KIND_READ);
    EXPECT_EQ(info.image_bytes, kImageBytes);
    EXPECT_EQ(info.utc, static_cast<uint64_t>(kUtc));
    EXPECT_EQ(info.rx_id, 0x7E8u);
    EXPECT_EQ(info.tx_id, 0x7E0u);
    EXPECT_EQ(info.range_count, 2u);
    EXPECT_EQ(info.ident_count, 2u);
    EXPECT_EQ(info.calibration_count, 2u);
    EXPECT_EQ(info.passes, 2u);
    EXPECT_EQ(info.seconds, 41u);
    EXPECT_EQ(info.volts_mv, 12450u);
    EXPECT_EQ(info.volts_min_mv, 12310u);
    EXPECT_EQ(info.rpm, 0u);
    EXPECT_EQ(info.kmh, 0u);
    EXPECT_EQ(info.unlocks, 1u);
    EXPECT_EQ(info.writes_before, 0u);
    EXPECT_EQ(info.reserved0, 0u);
    EXPECT_STREQ(info.core, "0.9.1");
    EXPECT_STREQ(info.plugin_id, "example-car");
    EXPECT_STREQ(info.plugin_version, "1.2.0");
    EXPECT_STREQ(info.operation, "dump-calibration");
    EXPECT_EQ(std::memcmp(info.module_sha256, r.module_sha256, 32), 0);
    EXPECT_EQ(std::memcmp(info.fingerprint, r.fingerprint, 32), 0);
    EXPECT_STREQ(info.scheme, "j2534");
    EXPECT_STREQ(info.adapter, "Made-up Adapter");
    EXPECT_STREQ(info.firmware, "1.0");
    EXPECT_STREQ(info.vin, "ZZZFAKE11M0000001");
    EXPECT_STREQ(info.ecu_name, "ECM-HavneSim");
    for (const uint32_t word : info.reserved) {
        EXPECT_EQ(word, 0u);
    }
    const Bytes image = image_of(kImageBytes);
    uint8_t sum[32];
    ecuio_sha256::hasher hash;
    hash.add(image.data(), image.size());
    hash.finish(sum);
    EXPECT_EQ(std::memcmp(info.image_sha256, sum, 32), 0);

    ecuio_dump_range_t range;
    std::memset(&range, 0xA5, sizeof range);
    range.struct_size = sizeof range;
    ASSERT_EQ(ecuio_dump_get_range(d.get(), 1, &range), ECUIO_OK);
    EXPECT_STREQ(range.name, "boot");
    EXPECT_EQ(range.step, ECUIO_DUMP_STEP_UPLOAD);
    EXPECT_EQ(range.session, ECUIO_DUMP_SESSION_PROGRAMMING);
    EXPECT_EQ(range.address, 0x00020000u);
    EXPECT_EQ(range.bytes, 256u);
    EXPECT_EQ(range.offset, 512u);
    EXPECT_EQ(range.flags, 0u);
    range.struct_size = sizeof range;
    ASSERT_EQ(ecuio_dump_get_range(d.get(), 0, &range), ECUIO_OK);
    EXPECT_STREQ(range.name, "calibration");
    EXPECT_EQ(range.step, ECUIO_DUMP_STEP_READ);
    EXPECT_EQ(range.session, ECUIO_DUMP_SESSION_EXTENDED);
    EXPECT_EQ(range.address, 0x00010000u);

    ecuio_dump_ident_t ident;
    std::memset(&ident, 0xA5, sizeof ident);
    ident.struct_size = sizeof ident;
    ASSERT_EQ(ecuio_dump_get_ident(d.get(), 1, &ident), ECUIO_OK);
    EXPECT_EQ(ident.did, 0xD221u);
    EXPECT_EQ(ident.role, ECUIO_DUMP_ROLE_BASE);
    ASSERT_EQ(ident.length, 3u);
    EXPECT_EQ(ident.value[0], 0x00);
    EXPECT_EQ(ident.value[1], 0xFF);
    EXPECT_EQ(ident.value[2], 0x7A);
    EXPECT_EQ(ident.value[3], 0x00) << "the rest zeroed";

    ecuio_dump_calibration_t cal;
    std::memset(&cal, 0xA5, sizeof cal);
    cal.struct_size = sizeof cal;
    ASSERT_EQ(ecuio_dump_get_calibration(d.get(), 0, &cal), ECUIO_OK);
    EXPECT_EQ(cal.flags, ECUIO_DUMP_HAS_CALID | ECUIO_DUMP_HAS_CVN);
    EXPECT_STREQ(cal.calid, "FAKECAL-01");
    EXPECT_EQ(cal.cvn[0], 0xDE);
    EXPECT_EQ(cal.cvn[3], 0xEF);
    cal.struct_size = sizeof cal;
    ASSERT_EQ(ecuio_dump_get_calibration(d.get(), 1, &cal), ECUIO_OK);
    EXPECT_EQ(cal.flags, ECUIO_DUMP_HAS_CALID) << "a calid with no cvn";
    EXPECT_STREQ(cal.calid, "FAKECAL-02");

    std::string name;
    EXPECT_EQ(verify(d, &name), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(name, name_for(bytes));
    EXPECT_EQ(name.rfind("20261122-7E0-", 0), 0u) << name;
}

TEST(DumpFileTest, TheImageIsReadInPiecesFromAnyOffset)
{
    Folder f;
    const Dump d = open_dump(f.put_copy(file_of(record_of())));
    ASSERT_NE(d, nullptr);
    const Bytes image = image_of(kImageBytes);
    Bytes got;
    uint8_t piece[100];
    for (;;) {
        size_t n = 99;
        ASSERT_EQ(ecuio_dump_read(d.get(), got.size(), piece, sizeof piece, &n),
                  ECUIO_OK);
        if (n == 0) {
            break;
        }
        got.insert(got.end(), piece, piece + n);
    }
    EXPECT_EQ(got, image) << "the last piece short, then none";

    size_t n = 99;
    EXPECT_EQ(
        ecuio_dump_read(d.get(), kImageBytes - 3, piece, sizeof piece, &n),
        ECUIO_OK);
    EXPECT_EQ(n, 3u) << "never into the record";
    EXPECT_EQ(Bytes(piece, piece + 3), Bytes(image.end() - 3, image.end()));
    EXPECT_EQ(ecuio_dump_read(d.get(), UINT64_MAX, piece, sizeof piece, &n),
              ECUIO_OK);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(ecuio_dump_read(nullptr, 0, piece, sizeof piece, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_read(d.get(), 0, nullptr, sizeof piece, &n),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_read(d.get(), 0, piece, sizeof piece, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
}

TEST(DumpFileTest, TheGettersCheckTheirArguments)
{
    Folder f;
    const Dump d = open_dump(f.put_copy(file_of(record_of())));
    ASSERT_NE(d, nullptr);
    ecuio_dump_info_t info;
    std::memset(&info, 0xA5, sizeof info);
    info.struct_size = sizeof info - 1;
    EXPECT_EQ(ecuio_dump_get_info(d.get(), &info), ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(info.format, 0xA5A5A5A5u) << "untouched";
    EXPECT_EQ(ecuio_dump_get_info(nullptr, &info), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_get_info(d.get(), nullptr), ECUIO_ERR_NULL_ARGUMENT);
    info.struct_size = sizeof info + 8;
    EXPECT_EQ(ecuio_dump_get_info(d.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.struct_size, sizeof info) << "this core's written back";

    ecuio_dump_range_t range;
    std::memset(&range, 0xA5, sizeof range);
    range.struct_size = sizeof range;
    EXPECT_EQ(ecuio_dump_get_range(d.get(), 2, &range),
              ECUIO_ERR_INVALID_ARGUMENT)
        << "at the count";
    EXPECT_EQ(range.step, 0xA5A5A5A5u);
    range.struct_size = sizeof range - 1;
    EXPECT_EQ(ecuio_dump_get_range(d.get(), 0, &range),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_dump_get_range(nullptr, 0, &range),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_get_range(d.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_dump_ident_t ident;
    std::memset(&ident, 0xA5, sizeof ident);
    ident.struct_size = sizeof ident;
    EXPECT_EQ(ecuio_dump_get_ident(d.get(), 2, &ident),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ident.did, 0xA5A5A5A5u);
    ident.struct_size = 0;
    EXPECT_EQ(ecuio_dump_get_ident(d.get(), 0, &ident),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(ecuio_dump_get_ident(nullptr, 0, &ident),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_dump_calibration_t cal;
    std::memset(&cal, 0xA5, sizeof cal);
    cal.struct_size = sizeof cal;
    EXPECT_EQ(ecuio_dump_get_calibration(d.get(), 2, &cal),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(cal.flags, 0xA5A5A5A5u);
    EXPECT_EQ(ecuio_dump_get_calibration(d.get(), 0, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);

    ecuio_dump_t* out = reinterpret_cast<ecuio_dump_t*>(&out);
    EXPECT_EQ(ecuio_dump_open(nullptr, &out), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(ecuio_dump_open("x", nullptr), ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_open(f.at("none.havnedump").c_str(), &out),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file can't be opened: there's no such file");
    EXPECT_EQ(std::string(ecuio_last_error_detail()).find(f.str()),
              std::string::npos)
        << "words name no path";
    ecuio_dump_close(nullptr);
}

TEST(DumpFileTest, ARecordWithNothingOptionalReadsBackToo)
{
    Folder f;
    ecuio_dump_file::record r = record_of();
    r.vin.clear();
    r.calids.clear();
    r.cvns.clear();
    r.ecu_name.clear();
    r.idents.clear();
    r.has_volts = r.has_volts_min = r.has_rpm = r.has_kmh = false;
    r.parked_tick = false;
    r.adapter.clear();
    r.firmware.clear();
    r.rx_id = 0x18DAF110;
    r.tx_id = 0x18DA10F1;
    const std::string bytes = file_of(r);
    EXPECT_EQ(text_of(r).find("\"obd\""), std::string::npos);
    EXPECT_EQ(text_of(r).find("\"rpm\""), std::string::npos)
        << "a reading nothing answered is left out, never 0";
    const Dump d = open_dump(f.put(name_for(bytes, kUtc, r.tx_id), bytes));
    ASSERT_NE(d, nullptr);
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    ASSERT_EQ(ecuio_dump_get_info(d.get(), &info), ECUIO_OK);
    EXPECT_EQ(info.flags, 0u);
    EXPECT_EQ(info.ident_count, 0u);
    EXPECT_EQ(info.calibration_count, 0u);
    EXPECT_EQ(info.tx_id, 0x18DA10F1u);
    EXPECT_STREQ(info.vin, "");
    EXPECT_STREQ(info.adapter, "");
    std::string name;
    EXPECT_EQ(verify(d, &name), ECUIO_OK) << ecuio_last_error_detail();
    EXPECT_EQ(name.rfind("20261122-18DA10F1-", 0), 0u) << name;
    EXPECT_EQ(name.size() + 1, 93u) << "the longest name and its NUL";
    EXPECT_LE(name.size() + 1, size_t{ECUIO_DUMP_NAME_MAX});
}

TEST(DumpFileTest, VerifyCatchesOneFlippedBitAnywhere)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    const std::string name = name_for(bytes);
    const size_t image = ecuio_dump_file::kHeaderBytes;
    const size_t record = image + kImageBytes;

    /* In the image: the file still opens, since an open reads no image */
    {
        const Dump d = open_dump(f.put(name, flipped(bytes, image + 300, 5)));
        ASSERT_NE(d, nullptr);
        std::string given;
        EXPECT_EQ(verify(d, &given), ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), kDamaged);
        EXPECT_EQ(given, "") << "a damaged file is given no name";
    }
    /* In the record, and in either of the header's checksums */
    EXPECT_EQ(open_refusal(f, flipped(bytes, record + 40), name),
              "its record isn't the one its header seals");
    EXPECT_EQ(open_refusal(f, flipped(bytes, 64 + 31, 7), name),
              "its record isn't the one its header seals");
    EXPECT_NE(open_refusal(f, flipped(bytes, 32, 0), name)
                  .find("the record's image checksum isn't the header's"),
              std::string::npos);

    /* Every bit of the header and every 97th bit after it: the open or
     * the verify refuses each */
    for (size_t bit = 0; bit < bytes.size() * 8;
         bit += bit < image * 8 ? 1 : 97) {
        const std::string path = f.put(name, flipped(bytes, bit / 8, bit % 8));
        ecuio_dump_t* raw = nullptr;
        if (ecuio_dump_open(path.c_str(), &raw) != ECUIO_OK) {
            ASSERT_EQ(raw, nullptr);
            continue;
        }
        const Dump d(raw);
        ASSERT_EQ(verify(d), ECUIO_ERR_LOAD_FAILED) << "bit " << bit;
    }
}

TEST(DumpFileTest, VerifyHoldsAFileToTheNameItWasOpenedUnder)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    const std::string name = name_for(bytes);
    {
        const Dump d = open_dump(f.put("my backup.havnedump", bytes));
        ASSERT_NE(d, nullptr);
        std::string given;
        EXPECT_EQ(verify(d, &given), ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), kRenamed);
        EXPECT_EQ(given, name) << "a sound file is told its own name";
    }
    {
        /* One digit of the hash part changed: a copy's form, another's
         * checksum */
        std::string other = name;
        char& digit = other[other.size() - 11];
        digit = digit == '0' ? '1' : '0';
        const Dump d = open_dump(f.put(other, bytes));
        ASSERT_NE(d, nullptr);
        std::string given;
        EXPECT_EQ(verify(d, &given), ECUIO_ERR_LOAD_FAILED);
        EXPECT_STREQ(ecuio_last_error_detail(), kRenamed);
        EXPECT_EQ(given, name);
    }
    {
        /* The date and the ID are labels: only the hash part is judged */
        std::string label = name;
        label.replace(0, 12, "19990101-123");
        const Dump d = open_dump(f.put(label, bytes));
        ASSERT_NE(d, nullptr);
        std::string given;
        EXPECT_EQ(verify(d, &given), ECUIO_OK) << ecuio_last_error_detail();
        EXPECT_EQ(given, name) << "the record is their truth";
    }
}

TEST(DumpFileTest, VerifyChecksItsArgumentsAndStopsWhenAsked)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    const Dump d = open_dump(f.put_copy(bytes));
    ASSERT_NE(d, nullptr);
    char name[ECUIO_DUMP_NAME_MAX] = "x";
    EXPECT_EQ(ecuio_dump_verify(nullptr, nullptr, nullptr, name, sizeof name),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_verify(d.get(), nullptr, nullptr, nullptr, 96),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_verify(d.get(), nullptr, nullptr, name,
                                ECUIO_DUMP_NAME_MAX - 1),
              ECUIO_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(name, "x") << "untouched";
    EXPECT_EQ(ecuio_dump_verify(d.get(), nullptr, nullptr, nullptr, 0),
              ECUIO_OK)
        << "no name asked for";

    int asked = 0;
    const ecuio_cancel_t stop = [](void* user) {
        ++*static_cast<int*>(user);
        return 1;
    };
    EXPECT_EQ(ecuio_dump_verify(d.get(), stop, &asked, name, sizeof name),
              ECUIO_ERR_CANCELED);
    EXPECT_EQ(asked, 1);
    EXPECT_STREQ(name, "");
    const ecuio_cancel_t go = [](void* user) {
        ++*static_cast<int*>(user);
        return 0;
    };
    asked = 0;
    EXPECT_EQ(ecuio_dump_verify(d.get(), go, &asked, name, sizeof name),
              ECUIO_OK);
    EXPECT_GE(asked, 1);
    EXPECT_EQ(std::string(name), name_for(bytes));
}

TEST(DumpFileTest, VerifyRefusesAFileChangedSinceItWasOpened)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    const std::string path = f.put_copy(bytes);
    const Dump d = open_dump(path);
    ASSERT_NE(d, nullptr);
    /* Another file of the same length where the image was read: the
     * header differs, since it seals another image */
    ecuio_dump_file::record r = record_of();
    r.seconds = 42;
    Bytes image = image_of(kImageBytes);
    image[0] ^= 0xFF;
    const std::string other =
        ecuio_dump_file::build(r, image.data(), image.size());
    ASSERT_EQ(other.size(), bytes.size());
    f.put(name_for(bytes), other);
    EXPECT_EQ(verify(d), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file changed since it was opened");
    f.put(name_for(bytes), bytes + "x");
    EXPECT_EQ(verify(d), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file changed since it was opened");
    /* Under the header it was opened with, a byte of the record, then of
     * the image, changed: neither is the one the header seals */
    f.put(name_for(bytes), flipped(bytes, bytes.size() - 1));
    EXPECT_EQ(verify(d), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), kDamaged);
    f.put(name_for(bytes), flipped(bytes, 128 + kImageBytes - 1));
    EXPECT_EQ(verify(d), ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(), kDamaged);
    f.put(name_for(bytes), bytes);
    EXPECT_EQ(verify(d), ECUIO_OK) << ecuio_last_error_detail();
}

int grow(void* user)
{
    std::ofstream f(fs::u8path(*static_cast<std::string*>(user)),
                    std::ios::binary | std::ios::app);
    f << "x";
    return 0;
}

/* A byte added while the file is read: every byte the read took is the
 * opened file's, and the file is no longer it */
TEST(DumpFileTest, VerifyRefusesAFileThatGrowsWhileItIsRead)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    std::string path = f.put_copy(bytes);
    const Dump d = open_dump(path);
    ASSERT_NE(d, nullptr);
    char name[ECUIO_DUMP_NAME_MAX];
    EXPECT_EQ(ecuio_dump_verify(d.get(), grow, &path, name, sizeof name),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_STREQ(ecuio_last_error_detail(),
                 "the file changed since it was opened");
    EXPECT_EQ(read_bytes(path), bytes + "x");
}

/* The entry of name in the folder's list */
ecuio_dump_entry_t entry_of(const std::vector<ecuio_dump_entry_t>& list,
                            const std::string& name)
{
    for (const ecuio_dump_entry_t& e : list) {
        if (name == e.name) {
            return e;
        }
    }
    ADD_FAILURE() << name << " isn't listed";
    return ecuio_dump_entry_t{};
}

std::vector<ecuio_dump_entry_t> list_of(const Folder& f)
{
    size_t count = 99;
    EXPECT_EQ(ecuio_dump_list(f.str().c_str(), nullptr, 0, &count), ECUIO_OK)
        << ecuio_last_error_detail();
    std::vector<ecuio_dump_entry_t> out(count + 1);
    for (ecuio_dump_entry_t& e : out) {
        std::memset(&e, 0xA5, sizeof e);
    }
    size_t again = 0;
    EXPECT_EQ(ecuio_dump_list(f.str().c_str(), out.data(), out.size(), &again),
              ECUIO_OK);
    EXPECT_EQ(again, count);
    EXPECT_EQ(out[count].state, 0xA5A5A5A5u) << "nothing past the count";
    out.resize(count);
    return out;
}

TEST(DumpFileTest, AFoldersListGivesEachFileItsState)
{
    Folder f;
    EXPECT_TRUE(list_of(f).empty());
    ecuio_dump_file::record r = record_of();
    r.writes_before = 3;
    const std::string bytes = file_of(r);
    const std::string name = name_for(bytes);
    f.put(name, bytes);
    f.put("renamed.havnedump", bytes);
    f.put("garbage.havnedump", "not a dump");
    f.put(name + ".partial-123-0", bytes.substr(0, 500));
    f.put("notes.txt", "another program's");
    f.put("image.bin", bytes);
    const std::string later = [&] {
        std::string b = bytes;
        b[8] = 2;
        return b;
    }();
    f.put("zz-later.havnedump", later);

    const std::vector<ecuio_dump_entry_t> list = list_of(f);
    ASSERT_EQ(list.size(), 5u) << "notes.txt and image.bin are no dump's";
    for (size_t i = 1; i < list.size(); ++i) {
        EXPECT_LT(std::string(list[i - 1].name), std::string(list[i].name))
            << "by name";
    }
    const ecuio_dump_entry_t copy = entry_of(list, name);
    EXPECT_EQ(copy.state, ECUIO_DUMP_STATE_COPY);
    EXPECT_EQ(copy.flags, 0u);
    EXPECT_EQ(copy.kind, ECUIO_DUMP_KIND_READ);
    EXPECT_EQ(copy.writes_before, 3u);
    EXPECT_EQ(copy.utc, static_cast<uint64_t>(kUtc));
    EXPECT_EQ(copy.image_bytes, kImageBytes);
    EXPECT_EQ(copy.file_bytes, bytes.size());
    EXPECT_EQ(copy.rx_id, 0x7E8u);
    EXPECT_EQ(copy.tx_id, 0x7E0u);
    EXPECT_STREQ(copy.words, "");
    for (const uint32_t word : copy.reserved) {
        EXPECT_EQ(word, 0u);
    }

    const ecuio_dump_entry_t renamed = entry_of(list, "renamed.havnedump");
    EXPECT_EQ(renamed.state, ECUIO_DUMP_STATE_INVALID);
    EXPECT_STREQ(renamed.words,
                 "it reads as a copy, but its name isn't a copy's");
    EXPECT_EQ(renamed.writes_before, 3u) << "its record is still shown";
    EXPECT_EQ(renamed.tx_id, 0x7E0u);

    const ecuio_dump_entry_t garbage = entry_of(list, "garbage.havnedump");
    EXPECT_EQ(garbage.state, ECUIO_DUMP_STATE_INVALID);
    EXPECT_STREQ(garbage.words, "it is shorter than a dump's header");
    EXPECT_EQ(garbage.kind, 0u);
    EXPECT_EQ(garbage.file_bytes, 10u);

    const ecuio_dump_entry_t partial = entry_of(list, name + ".partial-123-0");
    EXPECT_EQ(partial.state, ECUIO_DUMP_STATE_UNFINISHED);
    EXPECT_STREQ(partial.words, "a write that didn't finish left it");
    EXPECT_EQ(partial.file_bytes, 500u);

    const ecuio_dump_entry_t newer = entry_of(list, "zz-later.havnedump");
    EXPECT_EQ(newer.state, ECUIO_DUMP_STATE_INVALID);
    EXPECT_STREQ(newer.words, "it was made by a later Havne (format 2): this "
                              "one reads format 1");

    /* The serial ports' contract */
    ecuio_dump_entry_t one[1];
    size_t count = 0;
    EXPECT_EQ(ecuio_dump_list(f.str().c_str(), one, 1, &count), ECUIO_OK);
    EXPECT_EQ(count, 5u) << "all found, the first copied";
    EXPECT_EQ(std::string(one[0].name), std::string(list[0].name));
    EXPECT_EQ(ecuio_dump_list(nullptr, one, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_list(f.str().c_str(), one, 1, nullptr),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_list(f.str().c_str(), nullptr, 1, &count),
              ECUIO_ERR_NULL_ARGUMENT);
    EXPECT_EQ(ecuio_dump_list(f.at("none").c_str(), one, 1, &count),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(count, 0u);
    EXPECT_STREQ(ecuio_last_error_detail(), "the folder can't be read");
}

TEST(DumpFileTest, AListedNameTooLongForItsFieldIsCutAndSaysSo)
{
    Folder f;
    /* Two-byte characters and the suffix: 100 fit the field, 130 run past
     * it with the field's last byte inside a character */
    std::string name;
    for (int i = 0; i < 100; ++i) {
        name += "\xC3\xA6";
    }
    name += ".havnedump";
    {
        std::ofstream out(f.path / fs::u8path(name), std::ios::binary);
        out << "x";
        if (!out) {
            GTEST_SKIP() << "this file system holds no name that long";
        }
    }
    name.clear();
    for (int i = 0; i < 130; ++i) {
        name += "\xC3\xA6";
    }
    name += ".havnedump";
    {
        std::ofstream out(f.path / fs::u8path(name), std::ios::binary);
        out << "x";
        if (!out) {
            GTEST_SKIP() << "this file system holds no name that long";
        }
    }
    const std::vector<ecuio_dump_entry_t> list = list_of(f);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].flags, 0u) << "210 bytes fit";
    EXPECT_EQ(std::strlen(list[0].name), 210u);
    EXPECT_EQ(list[1].flags, ECUIO_DUMP_ENTRY_NAME_CUT);
    EXPECT_EQ(std::strlen(list[1].name), 254u) << "cut between characters";
    EXPECT_EQ(list[1].state, ECUIO_DUMP_STATE_INVALID);
}

/* Each allocation of a clean open, verify and list fails in turn: the
 * call answers OK or OUT_OF_MEMORY, throws nothing, and an OK is whole */
TEST(DumpFileTest, OpenVerifyAndListSurviveEveryAllocationFailing)
{
    Folder f;
    const std::string bytes = file_of(record_of());
    const std::string path = f.put_copy(bytes);
    f.put("garbage.havnedump", "not a dump");
    const std::string folder = f.str();
    const std::string want = name_for(bytes);
    unsigned long refused = 0;
    for (unsigned long n = 1;; ++n) {
        ecuio_dump_t* d = nullptr;
        char name[ECUIO_DUMP_NAME_MAX] = "";
        ecuio_dump_entry_t list[4];
        size_t count = 0;
        fail_new_on_this_thread(n);
        const ecuio_status_t open = ecuio_dump_open(path.c_str(), &d);
        const ecuio_status_t check =
            open == ECUIO_OK
                ? ecuio_dump_verify(d, nullptr, nullptr, name, sizeof name)
                : ECUIO_ERR_OUT_OF_MEMORY;
        const ecuio_status_t listed =
            ecuio_dump_list(folder.c_str(), list, 4, &count);
        fail_new_on_this_thread(0);
        for (const ecuio_status_t s : {open, check, listed}) {
            ASSERT_TRUE(s == ECUIO_OK || s == ECUIO_ERR_OUT_OF_MEMORY)
                << n << ": " << s;
        }
        if (open != ECUIO_OK) {
            ASSERT_EQ(d, nullptr) << n;
        }
        if (check == ECUIO_OK) {
            ASSERT_EQ(want, name) << n;
        } else {
            ASSERT_STREQ(name, "") << n;
        }
        if (listed == ECUIO_OK) {
            ASSERT_EQ(count, 2u) << n;
            ASSERT_EQ(list[1].state, ECUIO_DUMP_STATE_INVALID) << n;
            ASSERT_EQ(list[0].state, ECUIO_DUMP_STATE_COPY) << n;
        }
        ecuio_dump_close(d);
        refused +=
            (open != ECUIO_OK) + (check != ECUIO_OK) + (listed != ECUIO_OK);
        if (open == ECUIO_OK && check == ECUIO_OK && listed == ECUIO_OK) {
            break;
        }
        ASSERT_LT(n, 5000u) << "the loop should end";
    }
    EXPECT_GT(refused, 10u) << "the calls do allocate";
}

} // namespace
