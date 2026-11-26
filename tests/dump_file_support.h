// File/layer: tests/dump_file_support.h | test-only, C++17, Qt-free, every
//             platform; header-only
// Testing: UNIT: DumpFileTest (test_core)
//
// What the dump file's cases share (ecuio/dump.h, core/src/dump_file.h):
// a made-up record and image, a file built from them under the name core
// gives it, a folder for the case and a handle that closes itself.

#ifndef ECUIO_TESTS_DUMP_FILE_SUPPORT_H
#define ECUIO_TESTS_DUMP_FILE_SUPPORT_H

#include "dump_file.h"
#include "sha256.h"
#include "temp_name.h"

#include <gtest/gtest.h>
#include <ecuio/core.h>
#include <ecuio/dump.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace dump_file_test {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

/* 2026-11-22 01:52:25 UTC */
inline constexpr int64_t kUtc = 1795312345;

/* An image's bytes: a made-up pattern, each byte its place's own */
inline Bytes image_of(size_t n)
{
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>((i * 131u) ^ (i >> 8) ^ 0x5A);
    }
    return b;
}

/* A record every key of which is set: two ranges tiling 768 bytes, two
 * identification DIDs and the module's own identification. Every value
 * is made up: no car's, module's or adapter's. */
inline ecuio_dump_file::record record_of()
{
    ecuio_dump_file::record r;
    r.utc = kUtc;
    r.core = "0.9.1";
    ecuio_dump_file::range a;
    a.name = "calibration";
    a.address = 0x00010000;
    a.bytes = 512;
    a.offset = 0;
    ecuio_dump_file::range b;
    b.name = "boot";
    b.step = ECUIO_DUMP_STEP_UPLOAD;
    b.session = ECUIO_DUMP_SESSION_PROGRAMMING;
    b.address = 0x00020000;
    b.bytes = 256;
    b.offset = 512;
    r.ranges = {a, b};
    r.rx_id = 0x7E8;
    r.tx_id = 0x7E0;
    ecuio_dump_file::ident hw;
    hw.did = 0xD220;
    hw.role = ECUIO_DUMP_ROLE_HARDWARE;
    hw.value = {'H', 'W', '-', '1'};
    ecuio_dump_file::ident sw;
    sw.did = 0xD221;
    sw.role = ECUIO_DUMP_ROLE_BASE;
    sw.value = {0x00, 0xFF, 0x7A};
    r.idents = {hw, sw};
    r.vin = "ZZZFAKE11M0000001";
    r.calids = {"FAKECAL-01", "FAKECAL-02"};
    r.cvns = {{0xDE, 0xAD, 0xBE, 0xEF}};
    r.ecu_name = "ECM-HavneSim";
    r.plugin_id = "example-car";
    r.plugin_version = "1.2.0";
    r.operation = "dump-calibration";
    for (int i = 0; i < 32; ++i) {
        r.module_sha256[i] = static_cast<uint8_t>(i);
        r.fingerprint[i] = static_cast<uint8_t>(0xF0 + (i & 0x0F));
    }
    r.scheme = "j2534";
    r.adapter = "Made-up Adapter";
    r.firmware = "1.0";
    r.seconds = 41;
    r.has_volts = r.has_volts_min = r.has_rpm = r.has_kmh = true;
    r.volts_mv = 12450;
    r.volts_min_mv = 12310;
    r.parked_tick = true;
    r.unlocks = 1;
    return r;
}

inline constexpr size_t kImageBytes = 768;

/* A whole file for r and an image of record_of()'s length */
inline std::string file_of(const ecuio_dump_file::record& r)
{
    const Bytes image = image_of(kImageBytes);
    return ecuio_dump_file::build(r, image.data(), image.size());
}

/* The file for a record text written by hand: the header's lengths and
 * checksums are the text's and the image's own */
inline std::string file_with(const std::string& text, const Bytes& image)
{
    ecuio_dump_file::header h;
    h.image_bytes = image.size();
    h.record_bytes = static_cast<uint32_t>(text.size());
    ecuio_sha256::hasher hash;
    hash.add(image.data(), image.size());
    hash.finish(h.image_sha256);
    hash.add(text.data(), text.size());
    hash.finish(h.record_sha256);
    uint8_t head[ecuio_dump_file::kHeaderBytes];
    ecuio_dump_file::write_header(h, head);
    std::string file(reinterpret_cast<const char*>(head), sizeof head);
    file.append(reinterpret_cast<const char*>(image.data()), image.size());
    file.append(text);
    return file;
}

/* The record text build() writes for r over record_of()'s image */
inline std::string text_of(const ecuio_dump_file::record& r)
{
    const std::string file = file_of(r);
    return file.substr(ecuio_dump_file::kHeaderBytes + kImageBytes);
}

/* The name core gives a file of these bytes */
inline std::string name_for(const std::string& file, int64_t utc = kUtc,
                            uint32_t tx = 0x7E0)
{
    uint8_t sum[32];
    ecuio_sha256::hasher hash;
    hash.add(file.data(), file.size());
    hash.finish(sum);
    return ecuio_dump_file::name_of(utc, tx, sum);
}

/* One folder for a case */
struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path =
            fs::path(tmp != nullptr ? tmp : "/tmp") / temp_name("ecuio-dump");
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, ec);
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            /* a read-only copy is removed as any file on POSIX, not on
             * Windows */
            fs::permissions(e.path(), fs::perms::owner_all, ec);
        }
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string str() const { return path.generic_u8string(); }
    std::string at(const std::string& name) const
    {
        return (path / name).generic_u8string();
    }
    /* bytes written as name; its path */
    std::string put(const std::string& name, const std::string& bytes) const
    {
        std::ofstream f(path / name, std::ios::binary | std::ios::trunc);
        f << bytes;
        return at(name);
    }
    /* bytes written under the name core gives them; its path */
    std::string put_copy(const std::string& bytes) const
    {
        return put(name_for(bytes), bytes);
    }
};

inline std::string read_bytes(const std::string& path)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

struct DumpClose {
    void operator()(ecuio_dump_t* d) const { ecuio_dump_close(d); }
};
using Dump = std::unique_ptr<ecuio_dump_t, DumpClose>;

/* The file at path opened, the open expected to pass */
inline Dump open_dump(const std::string& path)
{
    ecuio_dump_t* d = nullptr;
    EXPECT_EQ(ecuio_dump_open(path.c_str(), &d), ECUIO_OK)
        << ecuio_last_error_detail();
    return Dump(d);
}

/* The words a refused open gives for bytes written as name */
inline std::string open_refusal(const Folder& f, const std::string& bytes,
                                const std::string& name = "x.havnedump")
{
    ecuio_dump_t* d = reinterpret_cast<ecuio_dump_t*>(&d);
    EXPECT_EQ(ecuio_dump_open(f.put(name, bytes).c_str(), &d),
              ECUIO_ERR_LOAD_FAILED);
    EXPECT_EQ(d, nullptr);
    return ecuio_last_error_detail();
}

} // namespace dump_file_test

#endif /* ECUIO_TESTS_DUMP_FILE_SUPPORT_H */
