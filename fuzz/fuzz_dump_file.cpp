// File/layer: fuzz/fuzz_dump_file.cpp | C++17, Qt-free, libFuzzer
// Testing: FUZZ: fuzz_dump_file
//
// Arbitrary bytes as a dump file (ecuio/dump.h, core/src/dump_file.h).
// The first byte picks the way in and the name: its low bit the file
// parsed from memory (0) or written to a scratch folder, then opened,
// listed and verified (1); its next two bits the name the file is given
// there (its own, another copy's, one that is no copy's). The rest is
// the file.

#include "dump_file.h"
#include "ecuio/core.h"
#include "ecuio/dump.h"
#include "sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h> /* _getpid */
#define ecuio_fuzz_getpid _getpid
#else
#include <unistd.h>
#define ecuio_fuzz_getpid getpid
#endif

namespace fs = std::filesystem;

namespace {

using ecuio_dump_file::header;
using ecuio_dump_file::judged;
using ecuio_dump_file::kHeaderBytes;
using ecuio_dump_file::record;

const char kImageWords[] = "its image isn't the one its header seals";

void check(bool ok)
{
    if (!ok) {
        abort();
    }
}

/* The scratch folder, one per process (libFuzzer's -jobs run several) */
std::string g_folder;

void remove_folder()
{
    std::error_code ec;
    fs::remove_all(fs::u8path(g_folder), ec);
}

const std::string& folder()
{
    if (g_folder.empty()) {
        const char* tmp = getenv("TEMP");
        if (tmp == nullptr) {
            tmp = getenv("TMPDIR");
        }
        std::string base = tmp != nullptr ? tmp : "/tmp";
        for (char& c : base) {
            if (c == '\\') {
                c = '/';
            }
        }
        while (base.size() > 1 && base.back() == '/') {
            base.pop_back();
        }
        g_folder = base + "/ecuio-fuzz-dump-" +
                   std::to_string(static_cast<long>(ecuio_fuzz_getpid()));
        std::error_code ec;
        fs::create_directories(fs::u8path(g_folder), ec);
        atexit(remove_folder);
    }
    return g_folder;
}

void sum_of(const uint8_t* p, size_t n, uint8_t out[32])
{
    ecuio_sha256::hasher h;
    h.add(p, n);
    h.finish(out);
}

/* A refusal's words: one line, and never the scratch folder's path */
void check_words(const std::string& why, bool input_holds_folder)
{
    check(!why.empty());
    check(why.find('\n') == std::string::npos);
    check(input_holds_folder || g_folder.empty() ||
          why.find(g_folder) == std::string::npos);
}

/* What an accepted file holds, by the harness's own reading of the
 * format: its sizes add to the file's, its ranges tile its image and
 * share no address, and its record's checksum is the header's */
void check_accepted(const uint8_t* file, size_t size, const header& h,
                    const record& r)
{
    check(h.format == ECUIO_DUMP_FORMAT);
    check(h.image_bytes >= 1 && h.image_bytes <= ECUIO_DUMP_IMAGE_MAX);
    check(h.record_bytes >= 1 && h.record_bytes <= ECUIO_DUMP_RECORD_MAX);
    check(size == kHeaderBytes + h.image_bytes + h.record_bytes);
    check(r.image_bytes == h.image_bytes);
    check(memcmp(r.image_sha256, h.image_sha256, 32) == 0);
    uint8_t sum[32];
    sum_of(file + kHeaderBytes + h.image_bytes, h.record_bytes, sum);
    check(memcmp(sum, h.record_sha256, 32) == 0);
    check(!r.ranges.empty() && r.ranges.size() <= ECUIO_DUMP_MAX_RANGES);
    uint64_t next = 0;
    for (size_t k = 0; k < r.ranges.size(); ++k) {
        const ecuio_dump_file::range& g = r.ranges[k];
        check(g.offset == next && g.bytes >= 1);
        check(g.address + g.bytes <= (uint64_t{1} << 32));
        next += g.bytes;
        for (size_t j = 0; j < k; ++j) {
            const ecuio_dump_file::range& o = r.ranges[j];
            check(g.address >= o.address + o.bytes ||
                  o.address >= g.address + g.bytes);
        }
    }
    check(next == h.image_bytes);
    check(r.idents.size() <= ECUIO_DUMP_MAX_IDENTS);
    check(r.passes == 2 && r.utc >= 0 && r.utc <= ecuio_dump_file::kMaxUtc);
}

/* The record the getters describe, built from their values alone */
record record_from(const ecuio_dump_t* d)
{
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    check(ecuio_dump_get_info(d, &info) == ECUIO_OK);
    record r;
    r.kind = info.kind;
    r.utc = static_cast<int64_t>(info.utc);
    r.core = info.core;
    r.image_bytes = info.image_bytes;
    memcpy(r.image_sha256, info.image_sha256, 32);
    r.rx_id = info.rx_id;
    r.tx_id = info.tx_id;
    for (uint32_t k = 0; k < info.range_count; ++k) {
        ecuio_dump_range_t g{};
        g.struct_size = sizeof g;
        check(ecuio_dump_get_range(d, k, &g) == ECUIO_OK);
        ecuio_dump_file::range out;
        out.name = g.name;
        out.step = g.step;
        out.session = g.session;
        out.address = g.address;
        out.bytes = g.bytes;
        out.offset = g.offset;
        r.ranges.push_back(out);
    }
    for (uint32_t k = 0; k < info.ident_count; ++k) {
        ecuio_dump_ident_t i{};
        i.struct_size = sizeof i;
        check(ecuio_dump_get_ident(d, k, &i) == ECUIO_OK);
        check(i.length <= ECUIO_DUMP_IDENT_MAX);
        ecuio_dump_file::ident out;
        out.did = i.did;
        out.role = i.role;
        out.value.assign(i.value, i.value + i.length);
        r.idents.push_back(out);
    }
    for (uint32_t k = 0; k < info.calibration_count; ++k) {
        ecuio_dump_calibration_t c{};
        c.struct_size = sizeof c;
        check(ecuio_dump_get_calibration(d, k, &c) == ECUIO_OK);
        if ((c.flags & ECUIO_DUMP_HAS_CALID) != 0) {
            r.calids.emplace_back(c.calid);
        }
        if ((c.flags & ECUIO_DUMP_HAS_CVN) != 0) {
            r.cvns.push_back(std::array<uint8_t, 4>{
                {c.cvn[0], c.cvn[1], c.cvn[2], c.cvn[3]}});
        }
    }
    r.vin = info.vin;
    r.ecu_name = info.ecu_name;
    r.plugin_id = info.plugin_id;
    r.plugin_version = info.plugin_version;
    r.operation = info.operation;
    memcpy(r.module_sha256, info.module_sha256, 32);
    memcpy(r.fingerprint, info.fingerprint, 32);
    r.scheme = info.scheme;
    r.adapter = info.adapter;
    r.firmware = info.firmware;
    r.passes = info.passes;
    r.seconds = info.seconds;
    r.has_volts = (info.flags & ECUIO_DUMP_HAS_VOLTS) != 0;
    r.has_volts_min = (info.flags & ECUIO_DUMP_HAS_VOLTS_MIN) != 0;
    r.has_rpm = (info.flags & ECUIO_DUMP_HAS_RPM) != 0;
    r.has_kmh = (info.flags & ECUIO_DUMP_HAS_KMH) != 0;
    r.volts_mv = info.volts_mv;
    r.volts_min_mv = info.volts_min_mv;
    r.rpm = info.rpm;
    r.kmh = info.kmh;
    r.parked_tick = (info.flags & ECUIO_DUMP_PARKED_TICK) != 0;
    r.unlocks = info.unlocks;
    r.writes_before = info.writes_before;
    return r;
}

/* A record's values built into a file again read back equal. The build
 * seals the image it is given, which a file with a damaged image doesn't
 * hold under its record's checksum. */
void check_rebuilt(const record& r, const uint8_t* image, size_t image_bytes)
{
    const std::string again = ecuio_dump_file::build(r, image, image_bytes);
    header h;
    record back;
    std::string why;
    check(ecuio_dump_file::parse(reinterpret_cast<const uint8_t*>(again.data()),
                                 again.size(), &h, &back, &why) == judged::ok);
    record want = r;
    memcpy(want.image_sha256, back.image_sha256, 32);
    check(ecuio_dump_file::same(want, back));
    check(back.image_bytes == r.image_bytes);
}

/* The file from memory, twice: the two readings agree */
judged parsed(const uint8_t* file, size_t size, header* h, record* r,
              std::string* why)
{
    const judged first = ecuio_dump_file::parse(file, size, h, r, why);
    header h2;
    record r2;
    std::string why2;
    check(ecuio_dump_file::parse(file, size, &h2, &r2, &why2) == first);
    check(*why == why2);
    if (first == judged::ok) {
        check(ecuio_dump_file::same(*r, r2));
        check(memcmp(h->image_sha256, h2.image_sha256, 32) == 0);
    }
    return first;
}

/* The image through ecuio_dump_read in pieces of an odd size: each
 * offset reads the file's own byte */
void check_reads(const ecuio_dump_t* d, const uint8_t* image, uint64_t bytes)
{
    uint8_t piece[251];
    uint64_t at = 0;
    while (at < bytes) {
        size_t n = 0;
        check(ecuio_dump_read(d, at, piece, sizeof piece, &n) == ECUIO_OK);
        check(n >= 1 && n <= sizeof piece && at + n <= bytes);
        check(n == sizeof piece || at + n == bytes);
        check(memcmp(piece, image + at, n) == 0);
        at += n;
    }
    size_t n = 7;
    check(ecuio_dump_read(d, bytes, piece, sizeof piece, &n) == ECUIO_OK);
    check(n == 0);
}

/* The name the file is given in the scratch folder */
std::string name_for(unsigned pick, const std::string& own)
{
    switch (pick) {
    case 0:
        return own;
    case 1:
        return "20261122-7E0-" + std::string(64, '0') + ".havnedump";
    case 2:
        return "plain.havnedump";
    default:
        return "20261122-7E0-" + std::string(64, 'a') +
               ".havnedump.partial-1-1";
    }
}

/* The file on the disk under name: opened twice, read, verified and
 * listed, each judged against the reading from memory */
void on_the_disk(const uint8_t* file, size_t size, unsigned pick,
                 bool input_holds_folder)
{
    header h;
    record r;
    std::string why;
    const judged from_memory = parsed(file, size, &h, &r, &why);
    const bool image_only =
        from_memory == judged::refused && why == kImageWords;
    uint8_t whole[32];
    sum_of(file, size, whole);
    const std::string own =
        from_memory == judged::ok || image_only
            ? ecuio_dump_file::name_of(r.utc, r.tx_id, whole)
            : "20261122-7E0-" + ecuio_sha256::hex(whole) + ".havnedump";
    const std::string name = name_for(pick, own);
    const std::string path = folder() + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    check(f != nullptr);
    check(size == 0 || fwrite(file, 1, size, f) == size);
    fclose(f);

    ecuio_dump_t* d = nullptr;
    ecuio_dump_t* d2 = nullptr;
    const ecuio_status_t opened = ecuio_dump_open(path.c_str(), &d);
    const std::string open_words = ecuio_last_error_detail();
    check(ecuio_dump_open(path.c_str(), &d2) == opened);
    /* The open judges the header and the record; the image is a verify's */
    check((opened == ECUIO_OK) == (from_memory == judged::ok || image_only));
    if (opened != ECUIO_OK) {
        check(opened == ECUIO_ERR_LOAD_FAILED && d == nullptr && d2 == nullptr);
        check_words(open_words, input_holds_folder);
    } else {
        check_accepted(file, size, h, r);
        const record got = record_from(d);
        check(ecuio_dump_file::same(got, r));
        check(ecuio_dump_file::same(got, record_from(d2)));
        check_reads(d, file + kHeaderBytes, h.image_bytes);
        check_rebuilt(got, file + kHeaderBytes,
                      static_cast<size_t>(h.image_bytes));
        char said[ECUIO_DUMP_NAME_MAX];
        const ecuio_status_t verdict =
            ecuio_dump_verify(d, nullptr, nullptr, said, sizeof said);
        /* Sound by the harness's own sums; the name its own only at pick 0 */
        check((verdict == ECUIO_OK) == (!image_only && pick == 0));
        check(image_only ? said[0] == '\0' : said == own);
        if (verdict != ECUIO_OK) {
            check(verdict == ECUIO_ERR_LOAD_FAILED);
            check_words(ecuio_last_error_detail(), input_holds_folder);
        }
    }
    ecuio_dump_close(d);
    ecuio_dump_close(d2);

    ecuio_dump_entry_t list[2];
    size_t count = 0;
    check(ecuio_dump_list(folder().c_str(), list, 2, &count) == ECUIO_OK);
    check(count == 1 && list[0].name == name && list[0].flags == 0);
    check(list[0].file_bytes == size);
    const uint32_t want = pick > 2 ? ECUIO_DUMP_STATE_UNFINISHED
                          : opened == ECUIO_OK && pick < 2
                              ? ECUIO_DUMP_STATE_COPY
                              : ECUIO_DUMP_STATE_INVALID;
    check(list[0].state == want);
    check((list[0].words[0] == '\0') == (want == ECUIO_DUMP_STATE_COPY));
    if (opened == ECUIO_OK && pick <= 2) {
        check(list[0].utc == static_cast<uint64_t>(r.utc) &&
              list[0].image_bytes == h.image_bytes &&
              list[0].tx_id == r.tx_id && list[0].rx_id == r.rx_id);
    }
    remove(path.c_str());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1) {
        return 0;
    }
    const uint8_t pick = data[0];
    const uint8_t* file = data + 1;
    const size_t n = size - 1;
    const std::string& scratch = folder();
    const bool holds =
        std::string(reinterpret_cast<const char*>(file), n).find(scratch) !=
        std::string::npos;
    if ((pick & 1) != 0) {
        on_the_disk(file, n, (pick >> 1) & 3, holds);
        return 0;
    }
    header h;
    record r;
    std::string why;
    const judged j = parsed(file, n, &h, &r, &why);
    if (j != judged::ok) {
        check_words(why, holds);
        return 0;
    }
    check_accepted(file, n, h, r);
    uint8_t sum[32];
    sum_of(file + kHeaderBytes, static_cast<size_t>(h.image_bytes), sum);
    check(memcmp(sum, h.image_sha256, 32) == 0);
    check_rebuilt(r, file + kHeaderBytes, static_cast<size_t>(h.image_bytes));
    return 0;
}
