// File/layer: core/src/dump_file.h | internal, C++17, Qt-free, every
//             platform; no file is touched here
// Testing: UNIT: DumpFileTest (test_core); FUZZ: fuzz_dump_file
//
// A dump file's layout (ecuio/dump.h holds the specification): the
// header's bytes, the record's reader and writer, and a copy's name.
// Everything here works on bytes in memory; dump_file.cpp reads the
// file, and the writer that seals a copy builds on the same calls, so
// what core writes is by construction what it reads.

#ifndef ECUIO_DUMP_FILE_H
#define ECUIO_DUMP_FILE_H

#include "ecuio/dump.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ecuio_dump_file {

constexpr uint8_t kMagic[8] = {0x89, 0x48, 0x56, 0x44, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr size_t kHeaderBytes = ECUIO_DUMP_HEADER_BYTES;
constexpr char kSuffix[] = ".havnedump";
/* The last second of the year 9999: the name's date keeps 8 digits */
constexpr int64_t kMaxUtc = 253402300799;
/* The record's bounds the public header doesn't name */
constexpr size_t kNameMax = 31; /* a range's name, a plan's id */
constexpr size_t kTextMax = 63; /* an adapter's name, its firmware */
constexpr size_t kSchemeMax = 15;
constexpr size_t kCalidMax = 16;
constexpr size_t kEcuNameMax = 20;
constexpr uint32_t kUnlocksMax = 64;
constexpr uint32_t kSpeedMax = 100000; /* rpm and km/h: a plain bound */
constexpr uint32_t kVoltsMax = 100000; /* millivolts */

struct header {
    uint32_t format = ECUIO_DUMP_FORMAT;
    uint64_t image_bytes = 0;
    uint32_t record_bytes = 0;
    uint8_t image_sha256[32] = {};
    uint8_t record_sha256[32] = {};
};

struct range {
    std::string name;
    uint32_t step = ECUIO_DUMP_STEP_READ;
    uint32_t session = ECUIO_DUMP_SESSION_EXTENDED;
    uint64_t address = 0;
    uint64_t bytes = 0;
    uint64_t offset = 0;
};

struct ident {
    uint32_t did = 0;
    uint32_t role = ECUIO_DUMP_ROLE_HARDWARE;
    std::vector<uint8_t> value;
};

/* A record's values: what record_text writes and read_record reads */
struct record {
    uint32_t kind = ECUIO_DUMP_KIND_READ;
    int64_t utc = 0;
    std::string core;
    uint64_t image_bytes = 0;
    uint8_t image_sha256[32] = {};
    std::vector<range> ranges;
    uint32_t rx_id = 0;
    uint32_t tx_id = 0;
    std::vector<ident> idents;
    std::string vin; /* "obd"'s four, each "" or empty for none */
    std::vector<std::string> calids;
    std::vector<std::array<uint8_t, 4>> cvns;
    std::string ecu_name;
    std::string plugin_id;
    std::string plugin_version;
    std::string operation;
    uint8_t module_sha256[32] = {};
    uint8_t fingerprint[32] = {};
    std::string scheme;
    std::string adapter;
    std::string firmware;
    uint32_t passes = 2;
    uint32_t seconds = 0;
    bool has_volts = false;
    bool has_volts_min = false;
    bool has_rpm = false;
    bool has_kmh = false;
    uint32_t volts_mv = 0;
    uint32_t volts_min_mv = 0;
    uint32_t rpm = 0;
    uint32_t kmh = 0;
    bool parked_tick = false;
    uint32_t unlocks = 0;
    uint32_t writes_before = 0;
};

enum class judged : uint8_t {
    ok,
    refused,       /* *why says what, in words that name no path */
    out_of_memory, /* *why untouched */
};

/* The header's 128 bytes into *out, judged against the size of the file
 * they came from: the magic, the format (a later one refused as that,
 * before anything else), the header's own length, the flags and the
 * reserved bytes, each length's bound, and 128 + N + M being the file's
 * size exactly. Allocates only a refusal's words. */
judged read_header(const uint8_t* bytes, uint64_t file_bytes, header* out,
                   std::string* why);

/* h as the header's 128 bytes */
void write_header(const header& h, uint8_t out[kHeaderBytes]);

/* The record's text into *out: the JSON, the format before the keys, the
 * keys exactly, each value's form and bound, "image" equal to the
 * header's, and the ranges tiling the image. */
judged read_record(const char* text, size_t size, const header& h, record* out,
                   std::string* why);

/* r as the record's text, which read_record reads back to equal values.
 * Throws std::bad_alloc only. */
std::string record_text(const record& r);

/* Whether two records hold equal values */
bool same(const record& a, const record& b);

/* The name core gives a copy: <yyyymmdd>-<tx>-<64 hex>.havnedump, the
 * date utc's in UTC, tx in 3 upper-case hex digits (8 past 11 bits) */
std::string name_of(int64_t utc, uint32_t tx_id, const uint8_t file_sha256[32]);

/* The hash part of a copy's name into out: false for a name that hasn't
 * the form <8 digits>-<3 or 8 upper-case hex>-<64 lower-case hex> and
 * the suffix */
bool name_hash(std::string_view name, uint8_t out[32]);

/* Whether name is a partial a stopped write left: a copy's suffix, then
 * ".partial-" and anything */
bool partial_name(std::string_view name);

/* A whole file's bytes for a record and its image, the record's "image"
 * and the header's checksums set from them: what the sealing writer puts
 * on the disk, made in memory for a test and a fuzzer. Throws
 * std::bad_alloc only. */
std::string build(record r, const uint8_t* image, size_t image_bytes);

/* A whole file in memory judged as the reader judges one on the disk,
 * its two inner checksums recomputed too */
judged parse(const uint8_t* bytes, size_t size, header* h, record* r,
             std::string* why);

} // namespace ecuio_dump_file

#endif /* ECUIO_DUMP_FILE_H */
