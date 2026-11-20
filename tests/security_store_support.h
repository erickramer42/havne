// File/layer: tests/security_store_support.h | test-only, C++17, Qt-free
// Testing: what SecurityStoreTest's two files share (test_core): a folder
//          of the case's own, an entry and a store's text made short, and
//          the rows of a refusal

#ifndef ECUIO_TESTS_SECURITY_STORE_SUPPORT_H
#define ECUIO_TESTS_SECURITY_STORE_SUPPORT_H

#include "security_store.h"

#include "temp_name.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

namespace security_store_test {

namespace fs = std::filesystem;
using namespace ecuio_security;

/* Made-up vehicles, addresses, levels and seconds: no car's or module's
 * own. kCar is kEntry's "vehicle". */
constexpr uint64_t kCar = 0x0123456789ABCDEF;
constexpr uint64_t kOtherCar = 0xFEDCBA9876543210;
constexpr uint32_t kRx = 0x7E8;
constexpr uint32_t kRxLong = 0x18DAF110;
constexpr int64_t kUtc = 1795132800;
constexpr char kName[] = "security-store.json";

/* One folder for a case: temp_name is one name for a case, so a second
 * Folder in it would clear the first */
struct Folder {
    fs::path path;
    Folder()
    {
        const char* tmp = std::getenv("TEMP");
        if (tmp == nullptr) {
            tmp = std::getenv("TMPDIR");
        }
        path = fs::path(tmp != nullptr ? tmp : "/tmp") /
               temp_name("ecuio-security-store");
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    std::string file() const { return (path / kName).generic_u8string(); }
    void write(const std::string& text) const
    {
        std::ofstream f(path / kName, std::ios::binary | std::ios::trunc);
        f << text;
    }
    std::string read() const
    {
        std::ifstream f(path / kName, std::ios::binary);
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    }
    size_t entries() const
    {
        size_t n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            (void)e;
            ++n;
        }
        return n;
    }
};

/* kCar's entry */
inline store_entry entry(uint32_t rx, uint8_t level, uint32_t failed,
                         int64_t last, int64_t pending = 0)
{
    store_entry e;
    e.vehicle = kCar;
    e.rx = rx;
    e.level = level;
    e.failed = failed;
    e.last_utc = last;
    e.pending_utc = pending;
    return e;
}

inline void expect_same(const store& a, const store& b)
{
    ASSERT_EQ(a.entries.size(), b.entries.size());
    for (size_t k = 0; k < a.entries.size(); ++k) {
        EXPECT_EQ(a.entries[k].vehicle, b.entries[k].vehicle) << k;
        EXPECT_EQ(a.entries[k].rx, b.entries[k].rx) << k;
        EXPECT_EQ(a.entries[k].level, b.entries[k].level) << k;
        EXPECT_EQ(a.entries[k].failed, b.entries[k].failed) << k;
        EXPECT_EQ(a.entries[k].last_utc, b.entries[k].last_utc) << k;
        EXPECT_EQ(a.entries[k].pending_utc, b.entries[k].pending_utc) << k;
    }
}

/* What store_read says of text: "" for a store that reads */
inline std::string refusal(const std::string& text)
{
    store s;
    std::string why;
    const store_io io = store_read(text.data(), text.size(), &s, &why);
    if (io == store_io::ok) {
        EXPECT_TRUE(why.empty()) << why;
        return "";
    }
    EXPECT_EQ(io, store_io::refused);
    EXPECT_TRUE(s.entries.empty()) << "a refused store holds nothing";
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(why.find('\n'), std::string::npos) << "one line";
    return why;
}

inline std::string with_entries(const std::string& entries)
{
    return "{\"havne_security_store\": 1,\n \"entries\": [" + entries + "]}";
}

/* The vehicle stands last, so the columns of the keys before it are the
 * ones a row counts */
constexpr char kEntry[] = "{\"rx\": \"7E8\", \"level\": 1, \"failed\": 2, "
                          "\"last_utc\": 1795132800, \"pending_utc\": 0, "
                          "\"vehicle\": \"0123456789ABCDEF\"}";

/* kEntry in a store, with from replaced by to */
inline std::string entry_with(const char* from, const char* to)
{
    std::string t = kEntry;
    const size_t at = t.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    t.replace(at, std::string(from).size(), to);
    return with_entries(t);
}

struct refused_row {
    std::string text;
    const char* why;
};

/* Each text is refused in its words at its place */
inline void expect_refused(std::initializer_list<refused_row> rows)
{
    for (const refused_row& r : rows) {
        EXPECT_EQ(refusal(r.text), r.why) << r.text;
    }
}

} // namespace security_store_test

#endif /* ECUIO_TESTS_SECURITY_STORE_SUPPORT_H */
